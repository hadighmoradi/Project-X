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
| N113-G62R18 | اعتبارسنجی محدود N111/N112/N113 با MSVC: موفقیت ۲۵۲ از ۲۵۲ تست؛ بستن محدودهٔ Operation-level در Spill/Reload و انتقال کار CFG Edge Bundle به N114. |

## نقطهٔ ادامهٔ فعلی

جدیدترین رکورد اعتبارسنجی G62R18 در ۲۴ نوامبر ۲۰۲۶ و روی مبنای Workspace محلی N113-G62R16 اجرا شد. سه مجموعهٔ متمرکز N111، N112 و N113 با MSVC 19.51.36248 و CMake 4.4.4 در مجموع ۲۵۲ از ۲۵۲ تست را با موفقیت گذراندند. این نتیجه محدودهٔ اعتبارسنجی Spill/Reload در سطح Operation-level از N113 را می‌بندد.

محدودهٔ مهندسی بعدی N114 است: یکپارچه‌سازی CFG Edge Bundle و edgeهای CFG. مقدار G11 همچنان ۰ از ۷ است و در G62R18 عملیات Edge Bundle insertion انجام نشد. Full Regression و اجرای End-to-End فایل PE تولیدشده همچنان تأیید نشده‌اند. جزئیات محدوده و شواهد در [رکورد اعتبارسنجی G62R18](G62R18_VALIDATION_FA.md) آمده است.

## شکاف‌های تاریخچه

اسناد Recovery ناقص‌بودن بعضی شواهد مربوط به نگاشت N1–N10 و بخش‌هایی از N17–N23، از جمله N19 و اعتبارسنجی Windows برای N23، را تصریح می‌کنند. این خلاصه شکاف‌ها را با حدس پر نمی‌کند. سوابق جزئی در آرشیو کاری نگهداری می‌شوند.
