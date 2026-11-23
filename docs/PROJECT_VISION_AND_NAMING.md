# Project vision and naming

## Why build a new language?

For a long time, I have wanted one language that brings together three things: programs that run quickly, small native executables, and code that is easy to write. I would like to combine the speed and compact output people expect from low-level languages with the readability of a high-level language.

That is my goal for the language, not a claim about the current compiler. The compiler is still experimental, and its speed, output size, and ease of use will need to be measured as the work continues.

## The final name is still open

For now, I call the project **X**. It is a placeholder. I would like people who follow the project to suggest names, talk through the ideas, and then vote. The name with the most votes at the end will replace X.

The repository is public and Discussions are enabled. Post one name idea per discussion in [Ideas](https://github.com/hadighmoradi/Project-X/discussions/categories/ideas) and briefly explain why you chose it. After discussing and shortlisting candidates, I will open a poll in [Polls](https://github.com/hadighmoradi/Project-X/discussions/categories/polls). The name with the most votes when the poll closes will replace X.

### Proposed process

1. Open one discussion for each name idea and briefly explain why you chose it.
2. Compare similar ideas and talk through the strongest options.
3. Put the shortlist in a GitHub poll.
4. When voting closes, use the top-voted name throughout the project.

## Platforms: the goal and the current state

My goal is to support **x86-32 (X32)** and **x86-64 (X64)** on Windows and Linux, with macOS as a later target. The current code is only part of that plan: the integrated path is Windows x64/COFF; Linux has ELF64 components and a few limited checks, but no proven source-to-ELF path; x86-32 is not implemented; and Mach-O output for macOS is not ready.

Here, X32 means 32-bit x86 and X64 means x86-64. AMD64 is the technical name used for the Windows x64 COFF path; it does not mean the project is limited to AMD processors.
