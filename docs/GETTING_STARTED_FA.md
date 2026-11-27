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

## دربارهٔ این مخزن

این مخزن سورس کامپایلر و هدف‌های آزمون توسعهٔ آن را در خود دارد. پیاده‌سازی فعلی آزمایشی است و هنوز یک بستهٔ کامل از زبان همراه با Runtime، کتابخانهٔ استاندارد، IDE و طراح بصری فرم‌ها محسوب نمی‌شود.

برای آشنایی با زبان و مسیر کامپایلر پیاده‌سازی‌شده، [طراحی زبان](LANGUAGE_DESIGN_FA.md)، [معماری](ARCHITECTURE.md) و [وضعیت فعلی پروژه](STATUS_FA.md) را ببینید.
