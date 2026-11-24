// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" __declspec(dllimport) int N34Return42();
extern "C" __declspec(dllimport) void ExitProcess(unsigned int exit_code);

extern "C" void n34_start() {
    const int result = N34Return42();
    ExitProcess(static_cast<unsigned int>(result));
}
