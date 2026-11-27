<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div dir="rtl">

# N114 v10.63 — همگام‌سازی سورس با main عمومی

**تاریخ ثبت:** ۲۷ نوامبر ۲۰۲۶  
**مخزن:** `hadighmoradi/Project-X`  
**شاخهٔ پیش‌فرض:** `main`

## نتیجهٔ انتشار

Snapshot سورس N114 v10.63 اکنون در شاخهٔ عمومی `main` ادغام شده است.

| مورد | مقدار تأییدشده |
|---|---|
| PR مستندات | [شمارهٔ ۶](https://github.com/hadighmoradi/Project-X/pull/6) — ادغام‌شده |
| PR سورس | [شمارهٔ ۷](https://github.com/hadighmoradi/Project-X/pull/7) — ادغام‌شده |
| کامیت شاخهٔ سورس | `6e802faf646a301f124026d5d0661796dd97080b` |
| Merge commit سورس در main | `320c8d737aa2dc7dd9989026c6ffa3781248e77b` |
| Merge commit مستندات | `0bf01d27afb0cd7df35b1506096a0524db15654d` |
| مسیرهای تغییرکرده در PR سورس | ۱۵ (۱۴ فایل اصلاح‌شده، ۱ فایل جدید) |
| تعداد فایل‌های manifest سورس | ۱۹۴ |
| SHA-256 بسته | `ADE6210D272EE09421EA858205DDFBC993E688EA730A9A71F5F75A8FBFC7EBEB` |

کاندید ۱۹۴ فایلی شامل تمام ۱۹۳ مسیر Source موجود به‌علاوهٔ فایل جدید `tests/n114_b13b_expression_branch_anchor_tests.cpp` بود. ممیزی نشان داد ۱۷۹ فایل موجود پس از بازگرداندن اعلان مجوز با Git blobهای مبنا تطبیق دارند و ۱۴ فایل موجود تغییر محتوایی واقعی دارند. بنابراین PR سورس دقیقاً ۱۵ مسیر را تغییر داد.

پیش از Commit، Hash بسته، ساختار ZIP، مقدار SHA-256 هر ۱۹۴ فایل، Git blob ID هر ۱۹۴ فایل Staged و تطبیق دقیق Diff پانزده‌مسیره بررسی شدند. Commit سورس Push شد و SHA بازخوانی‌شده از Branch راه‌دور با Commit محلی تطبیق داشت. GitHub ادغام موفق PR شمارهٔ ۷ را اعلام کرد.

## منشأ اعتبارسنجی — محدودیت مهم

نتیجهٔ Full Regression ویندوز/MSVC همچنان یک نتیجهٔ واقعی روی Workspace محلی است: موفقیت ۱۷۵٬۹۷۶ از ۱۷۵٬۹۷۶ در ۲۶ از ۲۶ Batch، با موفقیت ۹ از ۹ هدف Build و بدون خطا در هفت Source Hash Guard نام‌برده.

Runner اصلی Regression، SHA مربوط به Git commit سورس را **ثبت نکرده است**. بنابراین PASS تاریخی، **مدرک اجرای کل آزمون روی کامیت سورس** `6e802faf646a301f124026d5d0661796dd97080b` یا Merge commit آن یعنی `320c8d737aa2dc7dd9989026c6ffa3781248e77b` محسوب نمی‌شود. هفت Guard هویت کل درخت را ثابت نمی‌کنند. پیش از نسبت‌دادن PASS تاریخی به Revision منتشرشده، Build و اعتبارسنجی لازم Windows/MSVC باید روی Revision دقیق منتشرشده دوباره اجرا شوند.

وضعیت GitHub Checks که برای دو PR پرس‌وجو شد، هیچ Status Check یا Workflow Run مرتبطی برنگرداند. این به معنی CI موفق نیست.

## محدودهٔ فنی حفظ‌شده

این همگام‌سازی Gateهای فنی ثبت‌شده را تغییر نمی‌دهد: مقدار G9 همچنان `STATEMENT_EDGE_ONLY`، مقدار G10 برابر `DO_NOT_REBUILD`، مقدار G11 برابر `0/7`، مقدار `spill_call_crossing=true` و وضعیت XMM non-empty برابر `OPEN/BLOCKED_NOT_PROVEN_IMPOSSIBLE` است. این کار Edge Bundle insertion، رزرو SpillSlot، تغییر unwind/relocation یا زبان/type/cast/conversion، اجرای End-to-End فایل PE تولیدشده یا گواهی Release را اثبات نمی‌کند.

هیچ Release یا Tag ساخته نشد.

</div>
