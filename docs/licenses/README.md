# Third-party software and attribution

The full license texts listed here are retained for the sources and portable package. CMake fetches developer-build dependencies from their upstream repositories. The Windows package includes the host, portable Clang/LLD/recompiler and redistributable runtime components; this directory supplies their notices. Project-owned additions use the root [MIT license](../../LICENSE); original third-party copyright notices remain intact.

| Component | Role | License notice |
| --- | --- | --- |
| TGP482/XboxXBERecomp | Bundled SDK, locally modified for this game | [MIT](../../sdk/LICENSE) |
| Zydis | XBE symbol scanning/recompiler | [MIT](Zydis-MIT.txt) |
| Zycore | Zydis dependency | [MIT](Zycore-MIT.txt) |
| toml++ | Configuration | [MIT](tomlplusplus-MIT.txt) |
| Dear ImGui | PC panel | [MIT](Dear-ImGui-MIT.txt) |
| SDL | Controllers | [zlib](SDL-zlib.txt) |
| SMAA | Shader and generic lookup tables | [MIT-style notice](SMAA-MIT.txt) |
| SCDA-XBERecomp | Graphics reference, first-launch compiler and separate DLL layout | [MIT](SCDA-XBERecomp-MIT.txt) |
| LLVM Clang/LLD | Portable first-launch compiler/linker | [Apache 2.0 with LLVM exceptions](LLVM-Apache-2.0-with-exceptions.txt) |
| Microsoft runtime components | Visual C++ runtime and D3D compiler DLLs in the Windows package | [Component notice](Microsoft-runtime-notice.txt) |

Additional original SDK notices are in [THIRD_PARTY.txt](../../sdk/THIRD_PARTY.txt). Copyright lines and license headers in source files remain intact. The notices below describe the graphics integration provenance.

- **SMAA:** shader e tabelas de área/busca em sdk/runtime/gpu/smaa_shader.h e smaa_textures.h. Componentes genéricos de [iryoku/smaa](https://github.com/iryoku/smaa), obtidos pelos headers do port SCDA no commit e8f8b443e97cc89c8b1f009df5b6b66e50a771ee. Os avisos originais foram mantidos nos dois arquivos. Licença completa: [SMAA-MIT.txt](SMAA-MIT.txt).
- **SCDA-XBERecomp:** referência do empacotamento C++ dos componentes e integração D3D12. Licença do repositório preservada em [SCDA-XBERecomp-MIT.txt](SCDA-XBERecomp-MIT.txt), copyright 2026 TGP482, Joshhhuaaa.

As tabelas SMAA são dados genéricos do algoritmo, não são texturas extraídas do jogo. A integração local usa os passes já existentes do SDK e um adaptador próprio. Não houve sincronização integral do SDK ou importação de hooks de jogo do port de referência.

## GTAO

A integração de horizontes em sdk/runtime/gpu/gtao_passes.h foi adaptada do kAoHlsl de SCDA-XBERecomp/src/gpu/d3d12.cpp no commit e8f8b443e97cc89c8b1f009df5b6b66e50a771ee. A licença MIT completa e o copyright 2026 TGP482, Joshhhuaaa estão no novo header e em [SCDA-XBERecomp-MIT.txt](SCDA-XBERecomp-MIT.txt). Reconstrução de profundidade/normais e filtragem foram integradas ao caminho local do Marvel. XeGTAO foi consultado como referência conceitual, sem importar seus arquivos nesta etapa.
