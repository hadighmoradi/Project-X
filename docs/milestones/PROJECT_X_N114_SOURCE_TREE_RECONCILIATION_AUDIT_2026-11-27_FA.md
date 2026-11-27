<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div dir="rtl">

# N114 v10.63 — ممیزی تطبیق درخت Source

**تاریخ:** ۲۷ نوامبر ۲۰۲۶  
**مبنای main عمومی:** `a387301b5efd7251a9346f823e2396748557743a`  
**کاندید:** آرشیو `XForm_test` پس از اعمال دو Patch پین‌شدهٔ N114 v10.63  
**وضعیت:** بسته آماده است؛ Push سورس و Validation متصل به Commit هنوز انجام نشده‌اند.

## تطبیق مسیرها و محتوا

درخت Source عمومی main شامل ۱۹۳ مسیر Trackشده در CMake، examples، headerها، source، tests و tools است. کاندید محلی ۱۹۴ مسیر دارد:

- هر ۱۹۳ مسیر موجود در main در کاندید حاضرند.
- هیچ مسیر Source اختصاصی main عمومی جا نمانده است.
- یک فایل جدید وجود دارد: `tests/n114_b13b_expression_branch_anchor_tests.cpp`.
- محتوای **۱۷۹ فایل از ۱۹۳ فایل موجود** پس از بازگرداندن اعلان‌های دقیق SPDX در main عمومی، با Git blob SHA-1 دقیقاً byte-for-byte تطبیق داده شد: ۱۷۲ فایل با Header GPL به‌اندازهٔ ۹۲ بایت و ۷ فایل شامل README فارسی example و شش Python verifier با اعلان‌های متناسب با مجوز فایل.
- **۱۴ فایل موجود** علاوه بر اعلان مجوز، تغییر محتوایی واقعی دارند:
  - `CMakeLists.txt`
  - `include/xfrontend/control_flow.hpp`
  - `include/xfrontend/native_cfg_bridge.hpp`
  - `include/xfrontend/x64_codegen.hpp`
  - `src/control_flow.cpp`
  - `src/native_cfg_bridge.cpp`
  - `src/x64_codegen.cpp`
  - `src/x64_location_transfer.cpp`
  - `tests/control_flow_tests.cpp`
  - `tests/n109a_cfg_edge_anchor_tests.cpp`
  - `tests/n54_register_codegen_tests.cpp`
  - `tests/n55_n59_backend_optimization_tests.cpp`
  - `tests/n72_n77_register_codegen_tests.cpp`
  - `tests/suite_main.cpp`

شش مورد از این ۱۴ تغییر محتوایی با Source Hash Guardهای نهایی N114 v10.63 پوشش دارند: CMake، دو فایل x64_codegen و تست‌های N54/N55/N72. هشت فایل دیگر از کاندید گسترده‌تر N113/CFG حفظ شده‌اند اما به‌صورت مستقل در هفت Guard نهایی پین نشده‌اند.

## بستهٔ انتقال آماده‌شده

- فایل: `ProjectX_N114_v10.63_SourceSyncBundle_20261127.zip`
- اندازه: ۸۰۸٬۸۶۹ بایت
- SHA-256: `ADE6210D272EE09421EA858205DDFBC993E688EA730A9A71F5F75A8FBFC7EBEB`
- محتوا: ۱۹۴ فایل انتخاب‌شدهٔ source/example/test/tool به‌همراه manifest کامل pre/post SHA-256 و Git-blob.
- پوشه‌های Build، گزارش‌ها، cacheها، executableها، backupها، آرشیوهای تو در تو و مواد خصوصی Recovery از بسته حذف شده‌اند.
- اعلان‌های SPDX بر اساس سیاست مجوز فایل‌های مخزن بازگردانده شده‌اند. هفت Hash ثبت‌شده در Runner ویندوز پیش از این اصلاح Header اندازه‌گیری شده‌اند؛ بنابراین Hash فایل‌های منتشرشده پس از Header متفاوت است، اما بدنهٔ کد حفظ شده است.

## مرز منشأ Source

نتیجهٔ Full Regression ویندوز/MSVC همچنان مدرک اجرای کاربر روی Workspace محلی است: ۱۷۵٬۹۷۶ از ۱۷۵٬۹۷۶، ۲۶ از ۲۶ Batch و ۹ از ۹ هدف Build. Runner، SHA مربوط به Git commit سورس را ثبت نکرد. کاندید آرشیو به‌همراه Patchها با دقت تطبیق داده شده، اما این موضوع به‌تنهایی ثابت نمی‌کند تمام فایل‌های بدون Guard در زمان اجرا byte-for-byte با Workspace دقیقاً آزموده‌شده یکسان بوده‌اند.

یک Branch مستقل با نام `sync/n114-v10.63-source-tree-2026-11-27` بر پایهٔ head آخرین Branch مستندات رزرو شده است. بسته باید از Windows روی آن Branch اعمال و Push شود. Commit دقیق حاصل باید manifest کامل و Build/Test لازم را دریافت کند؛ تنها پس از آن می‌توان ادعا کرد سورس عمومی همان سورس Regression-tested است. پس از Push و بازخوانی Branch، یک PR مستقل ایجاد خواهد شد.

```text
PUBLIC_MAIN_BASE=a387301b5efd7251a9346f823e2396748557743a
PUBLIC_TRACKED_SOURCE_PATHS=193
LOCAL_CANDIDATE_SOURCE_PATHS=194
EXISTING_PATHS_PRESENT=193
PUBLIC_ONLY_PATHS=0
HEADER_ONLY_GIT_BLOB_MATCHES=179
SUBSTANTIVE_EXISTING_SOURCE_CHANGES=14
N114_NEW_TEST_FILES=1
SOURCE_BUNDLE_FILES=194
SOURCE_BUNDLE_SIZE=808869
SOURCE_BUNDLE_SHA256=ADE6210D272EE09421EA858205DDFBC993E688EA730A9A71F5F75A8FBFC7EBEB
SOURCE_SYNC_BRANCH=sync/n114-v10.63-source-tree-2026-11-27
SOURCE_SYNC_BRANCH_STATUS=RESERVED_AWAITING_PUSH
SOURCE_SYNC_PR=NOT_CREATED
PUBLIC_SOURCE_SYNC=NOT_ESTABLISHED
```

</div>
