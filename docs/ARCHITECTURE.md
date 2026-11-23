# Project X architecture

The current workspace implements an experimental native frontend and a project-specific x64 (x86-64) backend.

```text
source text
  → parse_source
  → CompilationUnit
  → compile_x64_coff_program
  → AMD64 COFF modules + deterministic link plan
  → platform linker
```

`compile_x64_coff_program` consumes parsed `CompilationUnit` values; it does not perform parsing. The current Windows x64 code-generation path emits COFF objects. The platform linker creates the final PE32+ executable.

## Roadmap and implementation boundary

The owner-stated direction is x86-32 (X32) and x86-64 (X64), targeting Windows and Linux, with macOS planned later. The source evidence is narrower:

| Target | Current evidence |
|---|---|
| Windows x64 | Integrated compiler API emits AMD64 COFF objects and a link plan; the platform linker creates PE32+. |
| Linux x64 | ELF64 object/image components and scoped Linux-host LLD evidence exist; a complete source-to-ELF compiler API path is not established. |
| X32 / x86-32 | Not implemented in the current target model or code-generation path. “X32” here means 32-bit x86, not Linux x32 ABI. |
| macOS | Future target; metadata is modeled, but Mach-O serialization and macOS validation are not established. |

AMD64 is the conventional x86-64 name used by the current COFF machine type, not a restriction to AMD processors. Build-host support does not by itself prove generated-target support.

The active backend is custom x64 code, not LLVM. The current compiler workspace does not establish a complete language product, general-purpose CLI, full runtime/standard library, finished IDE, or production-ready GUI.
