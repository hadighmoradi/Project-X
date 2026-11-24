// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

module N113G18Emitter;
private function Helper(int p, int q) returns int { return p + q; }
private function FloatHelper(double x, double y) returns double { return x + y; }
private function Unused() returns int { return 77; }
public function Main() returns int {
    int a = 1; int b = 2; int c = 3; int d = 4; int e = 5; int f = 6;
    double x = 1.0; double y = 2.0; double z = 3.0; double w = 4.0;
    double total = FloatHelper(x + y, z + w);
    int i = 0; int result = 0;
    while (i < 4) {
        if (i < 2) { result = result + Helper(a, b); }
        else { result = result + Helper(c, d); }
        i = i + 1;
    }
    return result + e + f;
}
