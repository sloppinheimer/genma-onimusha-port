// Linux: the slice of the HLSL compiler the host uses (D3DCompile and its blobs). There's no d3dcompiler_47 on Linux:
// compat_shader.c supplies D3DCompile, on vkd3d-shader.
#pragma once
typedef struct ID3DBlob ID3DBlob;
typedef ID3DBlob ID3D10Blob;
typedef struct ID3DBlobVtbl {
    HRESULT (*QueryInterface)(ID3DBlob *This, REFIID riid, void **out);
    ULONG (*AddRef)(ID3DBlob *This);
    ULONG (*Release)(ID3DBlob *This);
    void *(*GetBufferPointer)(ID3DBlob *This);
    SIZE_T (*GetBufferSize)(ID3DBlob *This);
} ID3DBlobVtbl;
struct ID3DBlob { const ID3DBlobVtbl *lpVtbl; };
#define ID3D10Blob_Release(b) ((b)->lpVtbl->Release(b))
#define ID3D10Blob_GetBufferPointer(b) ((b)->lpVtbl->GetBufferPointer(b))
#define ID3D10Blob_GetBufferSize(b) ((b)->lpVtbl->GetBufferSize(b))
#define ID3DBlob_Release ID3D10Blob_Release
#define ID3DBlob_GetBufferPointer ID3D10Blob_GetBufferPointer
#define ID3DBlob_GetBufferSize ID3D10Blob_GetBufferSize
typedef struct { const char *Name, *Definition; } D3D_SHADER_MACRO;
typedef struct ID3DInclude ID3DInclude;
#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15)
#define D3DCOMPILE_SKIP_VALIDATION (1 << 1)
HRESULT D3DCompile(const void *src, SIZE_T len, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *inc,
                   const char *entry, const char *target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors);
