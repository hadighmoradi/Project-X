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
| N114 v10.63 | Full Regression روی Windows/MSVC در Workspace محلی ثبت شد (۱۷۵٬۹۷۶ از ۱۷۵٬۹۷۶؛ ۲۶ از ۲۶ Batch؛ ۹ از ۹ Build). سپس Snapshot گزینش‌شدهٔ ۱۹۴ فایلی از طریق PR شمارهٔ ۷ در main عمومی ادغام شد؛ منشأ اجرای Regression روی کامیت دقیق همچنان در انتظار اعتبارسنجی است. |

## نقطهٔ ادامهٔ فعلی

اکنون Snapshot سورس N114 v10.63 در `main` عمومی وجود دارد. کامیت شاخهٔ سورس `6e802faf646a301f124026d5d0661796dd97080b` از طریق PR شمارهٔ ۷ با Merge commit `320c8d737aa2dc7dd9989026c6ffa3781248e77b` ادغام شده است.

PASS تاریخی Full Regression همچنان مدرک اجرای Workspace محلی است: Runner، SHA مربوط به commit سورس را ثبت نکرد و هفت Source Guard نام‌برده، هویت کل درخت را ثابت نمی‌کنند. بنابراین کامیت دقیق سورس منتشرشده باید Build و Regression مستقل Windows/MSVC را دریافت کند تا آن PASS به آن قابل انتساب باشد. جزئیات در [رکورد همگام‌سازی main عمومی](milestones/PROJECT_X_N114_PUBLIC_MAIN_SYNC_2026-11-27_FA.md) و [رکورد Full Regression](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27_FA.md) آمده است.

Gateهای فنی N114 همچنان صریح‌اند: G9 فقط Statement-edge، مقدار G10 برابر DO_NOT_REBUILD، مقدار G11 برابر ۰ از ۷، مقدار `spill_call_crossing=true` و وضعیت XMM non-empty برابر open / blocked-not-proven-impossible است. ادعایی دربارهٔ Edge Bundle insertion یا اجرای End-to-End فایل PE تولیدشده مطرح نمی‌شود.

## شکاف‌های تاریخچه

اسناد Recovery ناقص‌بودن بعضی شواهد مربوط به نگاشت N1–N10 و بخش‌هایی از N17–N23، از جمله N19 و اعتبارسنجی Windows برای N23، را تصریح می‌کنند. این خلاصه شکاف‌ها را با حدس پر نمی‌کند. سوابق جزئی در آرشیو کاری نگهداری می‌شوند.
