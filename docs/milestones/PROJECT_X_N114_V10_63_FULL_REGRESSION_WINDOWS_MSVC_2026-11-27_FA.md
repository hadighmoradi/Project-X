<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div dir="rtl">

# Project X — عبور کامل Full Regression برای N114 v10.63 در Windows/MSVC

**تاریخ ثبت:** ۲۷ نوامبر ۲۰۲۶  
**وضعیت:** `PASS_FULL_REGRESSION`  
**منشأ شواهد:** گزارش‌های واقعی اجراشده توسط کاربر در Windows PowerShell 5.1 و ممیزی‌های تطبیق فقط‌خواندنی

## نتیجه

| معیار | نتیجه |
|---|---:|
| موجودی تعریف‌شده | **۱۷۵٬۹۷۶** |
| Batchهای معتبر | **۲۶ از ۲۶** |
| تست موفق / ناموفق | **۱۷۵٬۹۷۶ / ۰** |
| شناسه‌های تست یکتای سراسری | **۱۷۵٬۹۷۶** |
| Duplicate درون Batch / بین Batchها | **۰ / ۰** |
| ماتریس Build ویندوز | **۹ از ۹ هدف، کد خروجی صفر** |
| Source Hash Guard | **۷ از ۷، بدون خطا** |

این نتیجه تمام موجودی تعریف‌شده در برنامهٔ ۲۶-Batch را پوشش می‌دهد؛ نه تست‌های خارج از آن، همهٔ حالت‌های ABI/configuration، همهٔ قابلیت‌های کامپایلر یا گواهی Release را.

## مجموع Batchها

موجودی اصلی شامل ۱۷۵٬۰۰۹ تست در ۱۸ Batch است؛ هشت Suite تکمیلی نیز ۹۶۷ تست دارند.

| Batch | موفق | Batch | موفق |
|---|---:|---|---:|
| M01_AF | ۵۳۸ | M10_N7 | ۲۰٬۰۰۰ |
| M02_GM | ۱۴۲ | M11_N8A | ۱۲٬۰۰۰ |
| M03_OT | ۱٬۱۵۸ | M12_N8B | ۸٬۰۰۰ |
| M04_UZ | ۶۰۰ | M13_N90_N94 | ۱۰٬۰۰۰ |
| M05_N0_N2 | ۳۲٬۵۶۲ | M14_N95 | ۲٬۰۰۰ |
| M06_N3 | ۲۰٬۰۰۹ | M15_N96 | ۲٬۰۰۰ |
| M07_N4 | ۲۰٬۰۰۰ | M16_N97 | ۲٬۰۰۰ |
| M08_N5 | ۲۰٬۰۰۰ | M17_N98 | ۲٬۰۰۰ |
| M09_N6 | ۲۰٬۰۰۰ | M18_N99 | ۲٬۰۰۰ |

| Batch تکمیلی | موفق | Batch تکمیلی | موفق |
|---|---:|---|---:|
| L19_N102_N107 | ۶۰۱ | L23_N111 | ۲۳ |
| L20_N108 | ۲۳ | L24_N112 | ۲۵ |
| L21_N109A | ۳۴ | L25_N113 | ۲۰۴ |
| L22_N109B_N110A | ۳۷ | L26_N114_B13B | ۲۰ |

هر ۲۶ گزارش Attempt01 دارای Stage موفق، گزارش‌های TXT و JSON و Marker معتبر PASS بود.

## ماتریس Build ویندوز

هر ۹ هدف با کد خروجی صفر ساخته شدند و NMake به‌صورت Serial اجرا شد: `xfrontend_tests`، `xfrontend_tests_n102_n107`، `xfrontend_tests_n108`، `xfrontend_tests_n109a`، `xfrontend_tests_n109b_n110a`، `xfrontend_tests_n111`، `xfrontend_tests_n112`، `xfrontend_tests_n113` و `xfrontend_tests_n114_b13b`.

## اصلاحات سازگاری Regression

Patch فعال‌سازی Full Regression:
- دو مقداردهی قدیمی `NativeRegisterAllocationOptions` در N54 را اصلاح کرد تا مقدار پیش‌فرض false صریح باقی بماند؛
- انتظار اندازهٔ Frame در N56D را با درنظرگرفتن local/spill slotها، ذخیرهٔ GP/XMMهای callee-save و alignment در Win64 اصلاح کرد؛
- assertion مربوط به N73A را با معماری persistent-register در N96 هم‌راستا کرد؛
- مقدار `XFRONTEND_TEST_INVENTORY_TOTAL` را بدون تغییر target/source list از ۱۷۵٬۹۶۹ به ۱۷۵٬۹۷۶ رساند.

Validator در ابتدا ۱۸ Batch اصلی را به‌اشتباه رد می‌کرد، چون در اجرای فیلترشده تعداد تست‌های خارج از Batch انتخاب‌شده در مقدار Skipped می‌آید. رابطهٔ درست چنین است: `EXPECTED_SKIPPED = 175009 - EXPECTED_BATCH_COUNT`؛ هر ۱۸ مقدار تطبیق داشت. این کار تطبیق فقط‌خواندنی بود و هیچ تستی دوباره اجرا یا گزارشی بازنویسی نشد.

## هفت Source Hash Guard نهایی

| فایل | SHA-256 |
|---|---|
| `include/xfrontend/x64_codegen.hpp` | `C6FA4EC251FEA12C2D5D3FFCA97FC13D79303E854AB7B47FB014EACF15F646D0` |
| `src/x64_codegen.cpp` | `9015328B635C30FFEBD7AB7343C7197531E0245267F8A744547D5FF31CBFC3A8` |
| `tests/n114_b13b_expression_branch_anchor_tests.cpp` | `8B8F8AC665651AD76FFC5ED6655E1236303DFD1E72889C6BF55443809C4233AD` |
| `CMakeLists.txt` | `775854D8769DDD612477B5FF137E4B9237C36AC00B577FADEEBFC3AC06E8BC44` |
| `tests/n54_register_codegen_tests.cpp` | `749A4A85A4010EADD1685757E5CBCFB2D75BEE9DC231CE943EBC417B4BB5D672` |
| `tests/n55_n59_backend_optimization_tests.cpp` | `77096BB5419E47A6D29C4A7D0EE2F75DC953AD330ADEAA3CA0F38645ABDE3140` |
| `tests/n72_n77_register_codegen_tests.cpp` | `8D60C46B7C80D08ABB23AF9CBB46C59716D30D0CC27DCC8C554E7087A32E64F6` |

## مرزهای فنی

`G9=STATEMENT_EDGE_ONLY`؛ `G10=DO_NOT_REBUILD`؛ `G11=0/7`؛ `spill_call_crossing=true`؛ وضعیت XMM non-empty برابر `OPEN/BLOCKED_NOT_PROVEN_IMPOSSIBLE` است. این گزارش Edge Bundle insertion، رزرو SpillSlot، تغییر unwind/relocation یا زبان/type/cast/conversion و اجرای End-to-End فایل PE تولیدشده را اثبات نمی‌کند. این نتیجه گواهی Release نیست.

## منشأ شواهد و مرز سورس عمومی

گزارش‌ها توسط کاربر در یک Workspace محلی Windows/MSVC تولید شدند. Runner، SHA مربوط به Git commit سورس را ثبت نکرد. هفت Guard فقط هفت فایل نام‌برده را پوشش می‌دهند و هویت کل درخت Workspace را ثابت نمی‌کنند.

revision شاخهٔ عمومی `main` که در این به‌روزرسانی مستندات بررسی شد `a387301b5efd7251a9346f823e2396748557743a` بود. بررسی فقط‌خواندنی تفاوت درخت Source را نشان داد: فایل تست N114 B13-B در آن درخت عمومی وجود ندارد و اندازهٔ چند فایل اصلی متفاوت است. این سند یک PASS واقعی Full Regression روی Workspace محلی را ثبت می‌کند؛ ثابت نمی‌کند که `main` عمومی در آن زمان دقیقاً همان سورس آزموده‌شده را داشته است.

این تغییر مستنداتی عمداً زیرمجموعه‌ای تأییدنشده از فایل‌های سورس را وارد نمی‌کند. پیش از اینکه مخزن عمومی ادعا کند سورس Regression-tested را دارد، تطبیق و همگام‌سازی کامل Source باید جداگانه انجام شود.

```text
PROJECT_X_TEST_INVENTORY=175976
EXPECTED_BATCHES=26
VALIDATED_BATCHES=26
TOTAL_PASS=175976
TOTAL_FAIL=0
GLOBAL_UNIQUE_IDS=175976
GLOBAL_DUPLICATES=0
SOURCE_HASH_GUARD_FAILURES=0
WINDOWS_BUILD_MATRIX=PASS_9_OF_9
FULL_REGRESSION=PASS_FULL_REGRESSION
PUBLIC_MAIN_SOURCE_SYNC=NOT_ESTABLISHED
G9=STATEMENT_EDGE_ONLY
G10=DO_NOT_REBUILD
G11=0/7
spill_call_crossing=true
XMM_NONEMPTY=OPEN_BLOCKED_NOT_PROVEN_IMPOSSIBLE
```

</div>
