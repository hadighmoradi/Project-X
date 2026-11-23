# Current engineering status

**Checkpoint:** N113-G62R16  
**Recorded:** 23 November 2026

- The 14-file G62R16 source snapshot matches the corresponding live files.
- G62R13 passed an isolated MSVC G2-on probe on a controlled workload: 56 actions, 66 machine moves, and 474 transfer bytes.
- General edge insertion is not ready. G11 is 0/7, and edge-bundle insertion was not performed.
- Full regression was not run for this checkpoint.
- The next recorded task, G62R17, is to analyze expression-temporary liveness and register ownership against N112 Flow locations before designing opt-in routing.

The probe is bounded evidence. It does not establish general insertion readiness or a complete compiler product.



## Target platform roadmap

The owner-stated direction is x86-32 (X32) and x86-64 (X64) for Windows and Linux, with macOS planned later. Current code evidence supports a narrower statement:

| Target | Current status |
|---|---|
| Windows x64 | `compile_x64_coff_program` emits AMD64 COFF objects and a deterministic link plan; the platform linker produces PE32+. |
| Linux x64 | ELF64 components and scoped Linux-host LLD evidence exist; an end-to-end source-to-ELF compiler path is not established. |
| X32 / x86-32 | Not implemented in the current target model/backend. |
| macOS | Future; Mach-O serialization and macOS validation are not established. |

Here X32 means 32-bit x86, not Linux x32 ABI. AMD64 is the standard technical name for x86-64 in the current COFF path; it does not imply an AMD-only target. A C++ build working on a host is not evidence that this compiler can generate native output for that host's operating system.


## Community naming

X is the temporary working name; the final language name is open for community choice. The repository is public and Discussions are enabled. Post candidates in [Ideas](https://github.com/hadighmoradi/Project-X/discussions/categories/ideas); after discussion and shortlisting, I will open the poll in [Polls](https://github.com/hadighmoradi/Project-X/discussions/categories/polls). No final-name poll has been published yet. See [Project vision and naming](PROJECT_VISION_AND_NAMING.md).
