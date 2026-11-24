<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div dir="rtl">

# G62R18 — رکورد اعتبارسنجی محدود

**زمان ثبت:** ۲۴ نوامبر ۲۰۲۶، ساعت ۱۷:۵۱:۴۶ به وقت میزبان (UTC+03:30)  
**شناسهٔ اعتبارسنجی:** G62R18  
**محیط:** Workspace محلی XForm_test در Windows  
**مبنای سورس و Workspace:** N113-G62R16  
**نتیجهٔ کلی:** PASS — فقط اعتبارسنجی محدود

## ۱. محدوده

در G62R18 سه target آزمون CMake پیکربندی و ساخته شدند و هر مجموعه با انتخاب کامل تست‌های خودش (filter = all) اجرا شد:

- xfrontend_tests_n111
- xfrontend_tests_n112
- xfrontend_tests_n113

Runner شناسه‌های گزارش‌شده را با manifest ثبت تست در سورس مقایسه کرد و شواهد مربوط به Hash سورس، toolchain، Configure، Build، Console و گزارش‌های هر مجموعه را نگه داشت.

## ۲. نتایج آزمون

| مجموعه | کشف‌شده | اجراشده | موفق | ناموفق | ردشده | شناسهٔ یکتا | تکراری / مفقود / اضافه |
|---|---:|---:|---:|---:|---:|---:|---|
| N111 | ۲۳ | ۲۳ | ۲۳ | ۰ | ۰ | ۲۳ | ۰ / ۰ / ۰ |
| N112 | ۲۵ | ۲۵ | ۲۵ | ۰ | ۰ | ۲۵ | ۰ / ۰ / ۰ |
| N113 | ۲۰۴ | ۲۰۴ | ۲۰۴ | ۰ | ۰ | ۲۰۴ | ۰ / ۰ / ۰ |
| **مجموع** | **۲۵۲** | **۲۵۲** | **۲۵۲** | **۰** | **۰** | **۲۵۲** | **۰ / ۰ / ۰** |

کد خروجی هر سه executable برابر صفر بود. هر سه گزارش JSON دارای summary.status = passed هستند. هر سه فایل validation-comparison مقدار ManifestMatches = true و Status = PASS دارند. شناسه‌های گزارش‌شده با شناسه‌های ثبت‌شده در سورس برای همان مجموعه تطبیق دارند.

Metadata آزمون تعداد ۱۷۵٬۹۳۷ را به‌عنوان inventory کلی پروژه ثبت می‌کند. این عدد تعداد تست‌های اجراشده نیست. در G62R18 فقط ۲۵۲ تست سه target نام‌برده اجرا شدند؛ بقیهٔ inventory در این مرحله اجرا نشده است.

## ۳. محیط ساخت

| مشخصه | مقدار ثبت‌شده |
|---|---|
| میزبان | Windows x64 |
| کامپایلر | MSVC 19.51.36248 |
| Build system | CMake 4.4.4 |
| Generator | NMake Makefiles |
| پیکربندی | Debug |
| اطلاعات Debug | /Z7 |
| کد خروجی Configure | ۰ |
| کد خروجی Build | ۰ |
| targetهای ساخته‌شده | xfrontend_tests_n111، xfrontend_tests_n112، xfrontend_tests_n113 |

## ۴. یکپارچگی سورس

Runner شانزده فایل زیر را پیش از اجرا با SHA-256 مورد انتظار مقایسه کرد و پس از اجرا دوباره Hash آن‌ها را خواند. هر ۱۶ فایل موجود بودند، Hash مورد انتظار را داشتند و پس از اجرا نیز بدون تغییر ماندند.

| فایل | SHA-256 |
|---|---|
| CMakeLists.txt | DC96B81FCC86DA598998E9B344D0DE1D2C45DBAE95A3100E616F8D6216A6F9A6 |
| src/x64_codegen.cpp | AA16D6BA6B60D0E10221A5765A48BB8025E2580E4EF18845A980F4558ABBB508 |
| src/x64_location_transfer.cpp | 9A70861BBF4F981A0466E672E56AB49E03A955479768136DB889F5C79771FE8D |
| src/native_register_analysis.cpp | CED647CFD9411AC9B41C93EC998BF2A43FBD1B7526F9F36583B09AB8ABA14FE4 |
| src/native_cfg_bridge.cpp | 2DF8CC162DE91C6C5E67EC41F6B95274592B45E54F296FBE15E1A1C4B4141493 |
| src/linear_scan_allocator.cpp | 15B0FE93AFE6795F6A0DECF4F243C6E27A90280E0E3657B1A8A9DB9EF2DFBECC |
| include/xfrontend/x64_codegen.hpp | 16010E7CDCCDC47D57B64CD290A177C6EF8B33FA252FAB0AADC8A68672B3A0EE |
| include/xfrontend/x64_location_transfer.hpp | 802A6DBA2970441236D70064419DA1AC2BE59D155BB666B5007B266FCCCA1549 |
| include/xfrontend/native_register_analysis.hpp | BCCE375E12C7CA705E802186AF08B885751338C1E421CC3CB25134F26E337AF8 |
| include/xfrontend/native_cfg_bridge.hpp | A76753477D7D166307BE7D9C1B8CB6D241783F624DD551E15D8E01F80E791422 |
| include/xfrontend/linear_scan_allocator.hpp | 6B5543314FC59630A25B7A5440AA202C5210E35E10845341D3EE3FA8E5396B27 |
| tests/suite_main.cpp | EE7F90D64F3B3A28AA36942038627554A335A5B2D882A801437424D05FFC5F80 |
| tests/test_report.hpp | 4F66AE4AA595B40C8C332093D2E15BE27CE8712CFD875DFCE34BE9B58EE32032 |
| tests/n111_machine_location_plan_tests.cpp | 71450C24F0B5BA83008E9D68AEC431BBDAD9FC2184965270321D72F935B83C1C |
| tests/n112_flow_segment_plan_tests.cpp | 859C08AF8DE4853E0EA60E992A0C46C2D24F10DD8EE7E1A2E7E4F60AD25577CE |
| tests/n113_flow_spill_reload_resolution_tests.cpp | CF42B0976C8C38E6D8E2EB4A227A0B8397AA07DA31388E45CE37A5922A11F347 |

## ۵. بستهٔ شواهد

- نام بسته: ProjectX-G62R18-Evidence-20261124-175925-6136.zip
- SHA-256 فایل ZIP: 0E7C221B0490135CB05DD4192A1932BAA33A1552624F7AAEE97A89265796CA25
- سلامت آرشیو: آزمون CRC فایل ZIP موفق بود.
- Manifest جمع‌آوری: COMPLETE؛ تعداد ۳۱ فایل شواهد انتخاب‌شده کپی شد؛ فایل ضروری مفقود صفر؛ اختلاف Hash هنگام کپی صفر؛ یک فایل اختیاری موجود نبود.
- پوشهٔ شواهد محلی: reports/G62R18-N111-N112-N113-20261124-174853-6136/
- پوشهٔ Build محلی: build-g62r18-n111-n112-n113-msvc-z7/

بسته شامل نسخهٔ دقیق Runner، logهای CMake Configure و Build، خروجی شناسایی نسخهٔ MSVC و CMake، CMakeCache، CSVهای Hash قبل و بعد، manifest شناسه‌های تست سورس، گزارش‌های خام JSON/TXT، مقایسهٔ هر Suite، logهای Console و Manifest جمع‌آوری است.

## ۶. تصمیم محدودهٔ N113 و محدودیت‌های باقی‌مانده

شواهد G62R18 برای بستن محدودهٔ اعتبارسنجی Spill/Reload در سطح Operation-level در N113 کافی است. این نتیجه به معنی بسته‌شدن تمام کارهای N113/CFG یا آمادگی محصول نیست.

- آمادگی G11 همچنان ۰ از ۷ است.
- Edge Bundle insertion انجام نشده است.
- یکپارچه‌سازی CFG Edge Bundle و کارهای گسترده‌تر CFG به N114 منتقل شده‌اند.
- Full Regression اجرا نشده است.
- اجرای End-to-End فایل PE تولیدشده بخشی از این اعتبارسنجی نبود.

عدد ۱۷۵٬۹۳۷ در inventory گزارش CMake نباید به‌عنوان تعداد تست اجراشده معرفی شود.

## ۷. یادداشت دربارهٔ گزارش Runner

در سه خط نمایشی مربوط به مجموعه‌ها در summary.txt اشکال قالب‌بندی وجود دارد و placeholderهای قالب رشته به‌صورت خام نمایش داده شده‌اند. این ایراد محدود به همان ردیف‌های نمایشی است. خلاصهٔ JSON، گزارش‌های JSON/TXT هر Suite، فایل‌های validation-comparison و logهای Console نتایج را به‌درستی ثبت کرده‌اند و مبنای ممیزی قرار گرفتند. اصلاح قالب‌بندی Runner باید جداگانه انجام شود؛ این مشکل نتیجهٔ اجرای تست‌ها را تغییر نمی‌دهد.

## ۸. منشأ و تفسیر شواهد

Metadata اجرای آزمون Workspace محلی XForm_test را نشان می‌دهد. Runner مقدار GitOperations = NONE ثبت کرده و SHA یک commit سورس را ذخیره نکرده است. بنابراین این شواهد اجرای مشخص در Workspace محلی و ثبات شانزده فایل نام‌برده قبل و بعد از آزمون را اثبات می‌کنند؛ اما مستقلاً ثابت نمی‌کنند که تمام فایل‌های مخزن عمومی در یک commit مشخص با Workspace آزموده‌شده یکسان بوده‌اند.

در زمان اجرای آزمون هیچ تغییر سورس، commit، push یا عملیات remote انجام نشد. تغییرات مستنداتی که به این رکورد ارجاع می‌دهند، یک تغییر جداگانه و صرفاً مستنداتی هستند.

</div>