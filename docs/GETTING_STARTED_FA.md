<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# راهنمای شروع و ساخت

## پیش‌نیازها

- CMake نسخهٔ 3.16 یا بالاتر.
- کامپایلر C++17 که CMake پشتیبانی کند؛ مانند MSVC، GCC یا Clang.
- برای ساخت با MSVC در Windows، نصب Visual Studio همراه با workload مربوط به C++.

در پیکربندی سطح‌بالای CMake، دریافت خودکار package شخص ثالث تعریف نشده است.

## پیکربندی و ساخت هدف آزمون ثبت‌شده

فرمان‌ها را از ریشهٔ مخزن اجرا کنید:

```sh
cmake -S . -B build
cmake --build build --config Release --target xfrontend_tests
```

اجرای ورودی CTest:

```sh
ctest --test-dir build -C Release --output-on-failure
```

فایل CMake سطح‌بالا، `xfrontend_tests` را به‌عنوان ورودی CTest ثبت می‌کند. مخزن هدف‌های suite محدود و ابزارهای smoke نیز دارد؛ اجرای عادی CTest به معنی اجرای تمام scriptهای اعتبارسنجی میزبان یا workloadهای تاریخی نیست.

برای ساخت تمام targetهای پیکربندی‌شده، نام target را حذف کنید:

```sh
cmake --build build --config Release
```

## محدودهٔ شواهد ثبت‌شده

جدیدترین Full Regression ثبت‌شده برای Workspace محلی N114 v10.63 روی Windows/MSVC، تمام **۱۷۵٬۹۷۶** تست موجودی تعریف‌شده را با موفقیت گذرانده است. هر ۲۶ Batch و هر ۹ هدف ماتریس Build موفق بوده‌اند و شناسهٔ تست ناموفق یا تکراری گزارش نشده است. به [رکورد Full Regression](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27_FA.md) مراجعه کنید.

این شواهد مربوط به Workspace محلی کاربر است و اثبات نمی‌کند که درخت `main` عمومی در یک commit مشخص byte-for-byte با سورس آزموده‌شده یکسان باشد. Runner، SHA مربوط به commit سورس را ثبت نکرده و همگام‌سازی کامل سورس عمومی همچنان باقی است. این مستندات ادعا نمی‌کنند که سورس منتشرشدهٔ فعلی با همین نتیجه Build یا دوباره آزموده شده است. G9 همچنان Statement-edge-only، مقدار G10 همچنان DO_NOT_REBUILD و مقدار G11 برابر ۰ از ۷ است؛ اجرای End-to-End فایل PE تولیدشده نیز اثبات نشده است.
