// Linux: D3DCompile (d3dcompiler.h) on vkd3d-shader, Wine's HLSL compiler, which writes Direct3D 9 bytecode for the
// profiles the host compiles (vs_2_0, vs_2_a, ps_2_0, ps_3_0). libvkd3d-shader.so.1 is loaded when first needed, from
// bin/ beside the executable; without it every compile fails and the host draws without that shader.
#include <windows.h>
#include <dlfcn.h>
#include <pthread.h>
#include <d3dcompiler.h>

void xlog(const char *fmt, ...);

// ---- the declarations used, from vkd3d_shader.h (vkd3d 1.19) ----------------------------------------------------
struct vk_code { const void *code; size_t size; };
struct vk_option { unsigned name, value; };
struct vk_compile_info {
    unsigned type; const void *next;
    struct vk_code source;
    unsigned source_type, target_type;
    const struct vk_option *options; unsigned option_count;
    unsigned log_level;
    const char *source_name;
};
struct vk_hlsl_info { unsigned type; const void *next; const char *entry_point; struct vk_code secondary_code; const char *profile; };
enum {
    VK_STRUCTURE_COMPILE_INFO = 0, VK_STRUCTURE_HLSL_SOURCE_INFO = 6,
    VK_SOURCE_HLSL = 2, VK_TARGET_D3D_BYTECODE = 4, VK_LOG_ERROR = 1,
    VK_OPTION_API_VERSION = 4, VK_API_VERSION_1_19 = 19,
};
static int (*vk_compile)(const struct vk_compile_info *info, struct vk_code *out, char **messages);
static void (*vk_free_code)(struct vk_code *code);
static void (*vk_free_messages)(char *messages);

static void load(void)
{
    void *lib = dlopen("libvkd3d-shader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) {
        vk_compile = dlsym(lib, "vkd3d_shader_compile");
        vk_free_code = dlsym(lib, "vkd3d_shader_free_shader_code");
        vk_free_messages = dlsym(lib, "vkd3d_shader_free_messages");
    }
    if (!vk_compile || !vk_free_code || !vk_free_messages) {
        vk_compile = 0;
        xlog("[d3d] no HLSL compiler (libvkd3d-shader.so.1: %s): drawing without shaders", lib ? "incomplete" : dlerror());
    }
}

// ---- blobs: the compiled code, or the compiler's messages (with a 0 after them, as the host prints them) ---------
struct blob { ID3DBlob iface; LONG refs; SIZE_T size; unsigned char data[]; };
static HRESULT blob_query(ID3DBlob *b, REFIID riid, void **out) { (void)b; (void)riid; if (out) *out = 0; return E_NOINTERFACE; }
static ULONG blob_addref(ID3DBlob *b) { return (ULONG)InterlockedIncrement(&((struct blob *)b)->refs); }
static ULONG blob_release(ID3DBlob *b)
{
    struct blob *x = (struct blob *)b;
    LONG n = InterlockedDecrement(&x->refs);
    if (!n) free(x);
    return (ULONG)n;
}
static void *blob_pointer(ID3DBlob *b) { return ((struct blob *)b)->data; }
static SIZE_T blob_size(ID3DBlob *b) { return ((struct blob *)b)->size; }
static const ID3DBlobVtbl blob_vtbl = {blob_query, blob_addref, blob_release, blob_pointer, blob_size};
static ID3DBlob *new_blob(const void *p, size_t n)
{
    struct blob *b = malloc(sizeof *b + n + 1);
    if (!b) return 0;
    b->iface.lpVtbl = &blob_vtbl;
    b->refs = 1;
    b->size = n;
    memcpy(b->data, p, n);
    b->data[n] = 0;
    return &b->iface;
}

// The host passes no macros or include handler, and no flags (d3dcompiler's defaults: column-major matrices).
HRESULT D3DCompile(const void *src, SIZE_T len, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *inc,
                   const char *entry, const char *target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)defines; (void)inc; (void)flags1; (void)flags2;
    if (code) *code = 0;
    if (errors) *errors = 0;
    pthread_once(&once, load);
    if (!vk_compile) return E_NOTIMPL;
    struct vk_hlsl_info hlsl = {VK_STRUCTURE_HLSL_SOURCE_INFO, 0, entry, {0, 0}, target};
    struct vk_option api = {VK_OPTION_API_VERSION, VK_API_VERSION_1_19};
    struct vk_compile_info info = {VK_STRUCTURE_COMPILE_INFO, &hlsl, {src, len}, VK_SOURCE_HLSL, VK_TARGET_D3D_BYTECODE,
                                   &api, 1, VK_LOG_ERROR, name};
    struct vk_code out = {0, 0};
    char *messages = 0;
    int r = vk_compile(&info, &out, &messages);
    if (messages && *messages && errors) *errors = new_blob(messages, strlen(messages));
    if (messages) vk_free_messages(messages);
    if (r < 0) return E_FAIL;
    if (code && !(*code = new_blob(out.code, out.size))) r = -1;
    vk_free_code(&out);
    return r < 0 ? E_OUTOFMEMORY : S_OK;
}
