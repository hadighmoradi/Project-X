// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" __declspec(dllexport) int N33Add(int left, int right) {
    return left + right;
}

extern "C" __declspec(dllexport) int N33Multiply(int left, int right) {
    return left * right;
}
