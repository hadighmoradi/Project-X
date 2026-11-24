<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# ورودی کاری کنترل‌شدهٔ N113

فایل `N113_Controlled_Workload.src` نخستین برنامهٔ Source اجراییِ تعریف‌شده در Workspace برای پیشبرد N113 است.

## هدف

این فایل برای عبور دادن یک برنامهٔ واقعیِ نوشته‌شده با Syntax زبان X از مسیر موجود `parse_source()` و `compile_x64_coff_program()` و سپس استخراج شواهد G18 ساخته شده است. برنامه عمداً چند Function، محاسبات GP و floating-point، فراخوانی تابع، حلقه و شاخهٔ شرطی دارد تا Edgeها و عملیات Flow از متن Source واقعی خود زبان حاصل شوند.

## جایگاه شواهد

- این فایل یک **Project-owned controlled workload** است.
- این فایل IR Fixture نیست و از C++ test literal استخراج نشده است.
- این فایل **برنامهٔ کاربر یا Product Source بیرونی نیست**؛ بنابراین وجود آن وضعیت `PRODUCT_SOURCE=PROVEN` را به‌تنهایی مجاز نمی‌کند.
- مسیر G18 برای whole-program census به `module N113ControlledWorkload;` نیاز دارد. این نیاز مخصوص Resolver/G18 است و Header عمومی اجباری برای همهٔ `.src`ها ایجاد نمی‌کند.
- موفقیت Parse/Codegen یا وجود Edgeها در این workload، به‌تنهایی اثبات liveness/ownership محصول بیرونی یا اجازهٔ insertion نیست.

## اجرای استاندارد

Runner موجود `run_n113_g18_actual_input_census_msvc_z7.ps1` باید از ریشهٔ Workspace با `-SourcePaths` به این فایل اجرا شود. Patch N113-G30 فقط همین نمونه و اسنادش را اضافه می‌کند؛ هیچ فایل C++، CMake، Fixture موجود یا منطق Allocator/Emitter را تغییر نمی‌دهد.
