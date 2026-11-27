<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div dir="rtl">

# وضعیت مهندسی جاری

**جدیدترین رکورد اعتبارسنجی:** N114 v10.63 · ۲۷ نوامبر ۲۰۲۶  
**نتیجه:** `PASS_FULL_REGRESSION` برای موجودی تعریف‌شدهٔ ۱۷۵٬۹۷۶ تست  
**محیط شواهد:** اجرای کاربر روی Windows x64 · MSVC · NMake به‌صورت Serial

## نتیجهٔ Full Regression

| معیار | نتیجه |
|---|---:|
| موجودی تعریف‌شده / موفق | **۱۷۵٬۹۷۶ از ۱۷۵٬۹۷۶** |
| Batchهای معتبر | **۲۶ از ۲۶** |
| تست ناموفق | **۰** |
| شناسه‌های یکتای سراسری | **۱۷۵٬۹۷۶** |
| Duplicate درون Batch / بین Batchها | **۰ / ۰** |
| ماتریس Build ویندوز | **۹ از ۹ هدف، کد خروجی صفر** |
| Source Hash Guard | **۷ از ۷، بدون خطا** |

این نتیجه تمام موجودی تعریف‌شدهٔ CMake را که در برنامهٔ ۲۶-Batch آمده است پوشش می‌دهد. این نتیجه ادعای پوشش موارد خارج از آن موجودی یا گواهی همهٔ targetها، حالت‌های ABI/configuration، قابلیت‌ها یا فایل‌های اجرایی تولیدشده نیست.

## همگام‌سازی Source و منشأ اعتبارسنجی

Snapshot سورس N114 v10.63 اکنون در `main` عمومی قرار دارد. کامیت شاخهٔ سورس `6e802faf646a301f124026d5d0661796dd97080b` از طریق [PR شمارهٔ ۷](https://github.com/hadighmoradi/Project-X/pull/7) ادغام شد و Merge commit آن `320c8d737aa2dc7dd9989026c6ffa3781248e77b` است. بخش مستندات نیز از طریق [PR شمارهٔ ۶](https://github.com/hadighmoradi/Project-X/pull/6) ادغام شد.

کاندید Source شامل ۱۹۴ ورودی manifest بود: ۱۹۳ مسیر Source موجود و یک تست جدید N114 B13-B. PR سورس دقیقاً ۱۵ مسیر را تغییر داد (۱۴ اصلاح و یک فایل جدید). SHA-256 بسته، ساختار ZIP، SHA-256 هر ۱۹۴ فایل، Git blob ID هر ۱۹۴ فایل Staged و Diff دقیق ۱۵ مسیره، پیش از Commit از Guardها عبور کردند. پیش از Merge نیز SHA شاخهٔ دوردست با Commit محلی یکسان بود.

**نتیجهٔ Full Regression تاریخی هنوز به کامیت سورس منتشرشده قابل انتساب نیست.** آزمون در Workspace محلی Windows/MSVC کاربر اجرا شد و Runner، SHA مربوط به Git commit سورس را ثبت نکرد. هفت Hash Guard فایل، هویت کامل درخت سورس را ثابت نمی‌کنند. پیش از ادعای اعمال PASS شامل ۱۷۵٬۹۷۶ تست به این Revision عمومی، Build و اعتبارسنجی لازم Windows/MSVC باید روی کامیت دقیق `6e802faf646a301f124026d5d0661796dd97080b` دوباره اجرا شود.

پرس‌وجوی GitHub Status هیچ Commit Status Check و هیچ Workflow Run مرتبط با PRها برنگرداند؛ بنابراین ادعای CI موفق مطرح نمی‌شود. جزئیات در [رکورد همگام‌سازی main عمومی](milestones/PROJECT_X_N114_PUBLIC_MAIN_SYNC_2026-11-27_FA.md)، [ممیزی درخت Source](milestones/PROJECT_X_N114_SOURCE_TREE_RECONCILIATION_AUDIT_2026-11-27_FA.md) و [رکورد Full Regression](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27_FA.md) آمده است.


## مرزهای فنی N114

- `G9=STATEMENT_EDGE_ONLY`.
- `G10=DO_NOT_REBUILD`.
- `G11=0/7`.
- `spill_call_crossing=true`.
- وضعیت XMM non-empty همچنان `OPEN/BLOCKED_NOT_PROVEN_IMPOSSIBLE` است.
- این رکورد اثبات‌کنندهٔ Edge Bundle insertion، رزرو SpillSlot، تغییر unwind/relocation یا تغییر زبان/type/cast/conversion نیست.
- اجرای End-to-End فایل PE تولیدشده جزو این اعتبارسنجی نبود.
- این PASS گواهی Release محسوب نمی‌شود.

</div>
