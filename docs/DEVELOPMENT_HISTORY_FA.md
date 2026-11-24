<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# تاریخچهٔ توسعه

این سند خلاصهٔ عمومی milestoneهایی است که در آرشیو پروژه ثبت شده‌اند. جای manifestهای هر patch و گزارش‌های اعتبارسنجی میزبان را نمی‌گیرد.

| بازهٔ milestone | محور ثبت‌شده |
|---|---|
| N1–N10 | پایه‌های اولیهٔ native frontend. |
| N11–N16 | گسترش regression و تحلیل ownership. |
| N17–N23 | رفتار drop، ABI، symbolها و کار روی binary object. |
| N24–N30 | توسعهٔ linker و خروجی PE32+. |
| N31–N38 | archive، import/export، DLL، TLS و پشتیبانی exception/unwind. |
| N39–N48 | قابلیت‌های پیشرفتهٔ PE64، debug و شروع code generation مستقیم. |
| N49–N65 | code generation بومی AMD64، بهینه‌سازی و تخصیص register. |
| N66–N89 | تحلیل register، control-flow graph و code generation چندبخشی. |
| N90–N101 | Flow value، stack home، memory phi و code generation پایدار GP/XMM. |
| N102–N107 / v1.48.0 | تخصیص CFG-aware و مقاوم‌سازی unwind. |
| N113-G1–G33 | شواهد FunctionEmitter و edge/operand، ابزارهای diagnostics و gateهای آمادگی؛ درج عمومی edge همچنان غیرفعال است. |
| N113-G57–G62R16 | تحلیل آمادگی و probe محدود، تطبیق سورس و ثبت snapshot چهارده‌فایلی. |

## نقطهٔ ادامهٔ فعلی

آخرین checkpoint برابر N113-G62R16 در ۲۳ نوامبر ۲۰۲۶ است. گام پیشنهادی G62R17 بررسی liveness موقت‌های expression و مالکیت register در برابر N112 Flow locations است تا پیش از طراحی routing اختیاری انجام شود. این مرحله هنوز انجام نشده است.

## شکاف‌های تاریخچه

اسناد Recovery ناقص‌بودن بعضی شواهد مربوط به نگاشت N1–N10 و بخش‌هایی از N17–N23، از جمله N19 و اعتبارسنجی Windows برای N23، را تصریح می‌کنند. این خلاصه شکاف‌ها را با حدس پر نمی‌کند. سوابق جزئی در آرشیو کاری نگهداری می‌شوند.
