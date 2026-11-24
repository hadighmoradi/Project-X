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

جدیدترین اعتبارسنجی متمرکز ثبت‌شده G62R18 در ۲۴ نوامبر ۲۰۲۶ و روی مبنای Workspace محلی N113-G62R16 است. سه target آزمون CMake مربوط به N111، N112 و N113 با MSVC 19.51.36248 و CMake 4.4.4 ساخته شدند؛ هر ۲۵۲ تست انتخاب‌شده (۲۳ + ۲۵ + ۲۰۴) موفق بودند و هیچ شکست یا تست ردشده‌ای وجود نداشت.

این اجرا Full Regression نیست. عدد ۱۷۵٬۹۳۷ که به‌عنوان inventory کلی پروژه ثبت شده، metadata شمار inventory است و تعداد تست اجراشده نیست. مقدار G11 همچنان ۰ از ۷ است، Edge Bundle insertion انجام نشده و یکپارچه‌سازی گسترده‌تر CFG Edge Bundle به N114 منتقل شده است.

این شواهد به Workspace محلی نام‌برده و کنترل SHA-256 شانزده فایل پیش و پس از اجرا مربوط‌اند؛ به‌تنهایی ثابت نمی‌کنند که تمام درخت مخزن عمومی در یک commit مشخص byte-for-byte با آن Workspace یکسان است. برای جزئیات به [رکورد اعتبارسنجی G62R18](G62R18_VALIDATION_FA.md) و [وضعیت جاری](STATUS_FA.md) مراجعه کنید.
