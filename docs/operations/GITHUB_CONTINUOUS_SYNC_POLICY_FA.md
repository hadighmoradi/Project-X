<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div dir="rtl">

# سیاست همگام‌سازی مستمر Project X با GitHub

**تاریخ تصویب:** ۲۷ نوامبر ۲۰۲۶  
**مرجع تصویب:** دستور صریح و مستمر کاربر در ۲۷ نوامبر ۲۰۲۶

## اصل اصلی

GitHub پروژه باید با وضعیت واقعی و قابل انتشار Project X همگام بماند. پس از هر مرحلهٔ مهم طراحی، تغییر Source، Build، تست، Runner، Validation، ممیزی یا اجرای فنی، قابلیت انتشار نتیجه بررسی شود. اگر نتیجه قابل انتشار است، ثبت آن در مخزن و به‌روزرسانی صفحات عمومی بخشی از تکمیل همان کار است؛ نه مرحله‌ای اختیاری که به یادآوری دوباره نیاز داشته باشد.

## مجوز مستمر کاربر

کاربر مجوز مستمر ساخت Branch مستقل، Commit، Push و ایجاد/به‌روزرسانی Pull Request برای تغییرات مناسب Project X را داده است. برای این اقدامات شاخه‌ای درخواست اجازهٔ مجدد نکن.

این مجوز شامل Merge به `main`، نوشتن مستقیم روی `main`، انتشار Release/Tag، عملیات مخرب روی فایل/Branch، Force-push یا بازنویسی تاریخچه، تغییر permission/security یا انتشار دادهٔ محرمانه و حساس نیست. این موارد به تأیید جداگانه نیاز دارند.

روال پیش‌فرض: **Branch → Commit → Push → Pull Request؛ بدون Merge**.

## چه چیزهایی باید ثبت شوند؟

نتایج مناسب Build، تست، Regression، Marker/ID audit، Source hash guard، اصلاح Runner/Validator، تغییر Toolchain، تصمیم‌های فنی، محدودیت‌ها و شکست‌های مهم باید ثبت شوند. نتیجهٔ منفی یا inconclusive را با وضعیت واقعی‌اش نگه دار؛ شکست را پنهان یا به PASS تبدیل نکن.

اطلاعات محرمانه، حساس، موقت یا تأییدنشده نباید وارد صفحات عمومی شوند. در صورت نیاز علت عدم انتشار در Master خصوصی ثبت شود.

## صفحاتی که باید به‌روز شوند

بر حسب ارتباط، نسخه‌های انگلیسی و فارسی README، STATUS، تاریخچهٔ توسعه، راهنمای شروع و Build، طراحی زبان، روش توسعه و صفحات سایت مانند `index.html`، `language-design.html` و `project-guide.html` را هماهنگ به‌روز کن. برای نتیجه‌های مهم در `docs/milestones/` رکورد تاریخ‌دار بساز. تاریخچهٔ قبلی را overwrite نکن و لینک‌های داخلی/مسیرهای نسبی را بررسی کن.

## الزامات شواهد

رکورد عمومی باید تا حد امکان تاریخ، محدودهٔ دقیق، محیط و Toolchain، فرمان یا Runner، تعداد مورد انتظار/اجراشده/موفق/ناموفق/Skipped، وضعیت ID و Duplicate، Source revision یا نبود صریح commit provenance، Hashهای مرتبط و محدودیت‌ها را ثبت کند.

نتیجهٔ Workspace محلی فقط مدرک همان Workspace است. تا وقتی commit SHA و هویت کامل درخت Source ثابت نشده، ادعا نکن که `main` عمومی دقیقاً همان سورس آزموده‌شده را دارد. Hash Guard محدود، Hash کل درخت نیست. Build، Test، Patch Apply، Source audit و End-to-End نتایج متفاوتی‌اند. عدد inventory را تعداد تست اجراشده ننام و PASS محدود را گواهی Release معرفی نکن.

## روند اجباری پس از هر مرحله

۱. قابلیت انتشار، منشأ، محدوده و حساسیت نتیجه را بررسی کن.  
۲. `main` و PRهای موجود را ببین و از Branch/PR تکراری پرهیز کن.  
۳. Branch مستقل و یکتا بساز.  
۴. مستندات دو زبانه و صفحات سایت مرتبط را به‌روز کن؛ برای نتیجهٔ مهم Milestone بساز.  
۵. Commit و Push انجام بده.  
۶. Branch head، فایل‌های تغییرکرده، base/head SHA، لینک‌ها و وضعیت PR را از GitHub دوباره بخوان و بررسی کن.  
۷. تا وقتی provenance یا Validation لازم کامل نیست، PR را Draft نگه دار.  
۸. PR تأییدشده و محدودیت‌هایش را در Master و گزارش به کاربر ثبت کن.  
۹. هرگز خودکار Merge نکن.

اگر اتصال GitHub نتواند درخت کامل Source را منتقل کند، محدودیت را صریح بگو، کار مستنداتیِ پشتیبانی‌شده را انجام بده و در صورت نیاز بستهٔ معتبر و اسکریپت کامل PowerShell 5.1 تهیه کن. تا وقتی Branch/Commit/PR واقعاً از GitHub بازخوانی و تأیید نشده، ادعای همگام‌سازی نکن.

## وضعیت ثبت‌شده در ۲۷ نوامبر ۲۰۲۶

- مخزن: `hadighmoradi/Project-X`.
- PR #6: https://github.com/hadighmoradi/Project-X/pull/6 — باز، Draft و Merge نشده.
- Branch: `docs/n114-full-regression-record-2026-11-27`.
- Commit قبلی: `9859f0920c92670d14ef1ea13d2fa40bde987b7a`.
- PR #6 نتیجهٔ Full Regression محلی N114 v10.63 را ثبت می‌کند: ۱۷۵٬۹۷۶ از ۱۷۵٬۹۷۶، ۲۶ Batch از ۲۶ و ۹ هدف Build از ۹.
- همگام‌سازی کامل Source با `main` عمومی هنوز اثبات نشده است؛ PR #6 فقط مستنداتی است.

```text
GITHUB_CONTINUOUS_SYNC=REQUIRED_WHEN_PUBLISHABLE
USER_STANDING_AUTHORIZATION_DATE=2026-11-27
BRANCH_COMMIT_PUSH_PR=AUTHORIZED_WITHOUT_REPEATED_PROMPT
DIRECT_MAIN_WRITE=NOT_AUTHORIZED
PR_MERGE=REQUIRES_SEPARATE_EXPLICIT_APPROVAL
RELEASE_OR_TAG=REQUIRES_SEPARATE_EXPLICIT_APPROVAL
BILINGUAL_DOCS_AND_SITE_SYNC=REQUIRED_WHEN_RELEVANT
EVIDENCE_PROVENANCE=REQUIRED
SOURCE_TREE_IDENTITY=DO_NOT_ASSUME
PUBLIC_SOURCE_SYNC=NOT_ESTABLISHED
```

</div>


## به‌روزرسانی انتشار — ۲۷ نوامبر ۲۰۲۶

مالک مخزن در گفت‌وگوی جاری صریحاً ادغام دو PR نام‌برده را تأیید کرد. این دو تغییر از مسیر Pull Request ادغام شدند و نوشتن مستقیم روی `main` انجام نشد:

- PR شمارهٔ ۶، مستندات و صفحات سایت — ادغام‌شده؛ Merge commit برابر `0bf01d27afb0cd7df35b1506096a0524db15654d`.
- PR شمارهٔ ۷، Snapshot سورس N114 v10.63 — ادغام‌شده؛ کامیت سورس `6e802faf646a301f124026d5d0661796dd97080b` و Merge commit برابر `320c8d737aa2dc7dd9989026c6ffa3781248e77b`.
- شاخهٔ عمومی `main` اکنون Diff پانزده‌مسیرهٔ سورس از کاندید curated شامل ۱۹۴ فایل را دارد.
- اعتبارسنجی دقیق Windows/MSVC روی Commit منتشرشده همچنان باقی است، چون Runner تاریخی SHA سورس را ثبت نکرد. پرس‌وجوی PRها هیچ GitHub Status Check یا Workflow Run برنگرداند. هیچ ادعای Release/Tag یا گواهی Release مطرح نشد.

روال پیش‌فرض همچنان Branch → Commit → Push → Pull Request است. Merge به تأیید جداگانه و مشخص کاربر نیاز دارد؛ تأیید ثبت‌شده در اینجا فقط شامل PRهای شمارهٔ ۶ و ۷ بود.

```text
PUBLIC_SOURCE_SYNC=INTEGRATED_IN_MAIN
SOURCE_COMMIT=6e802faf646a301f124026d5d0661796dd97080b
SOURCE_MERGE_COMMIT=320c8d737aa2dc7dd9989026c6ffa3781248e77b
DOCS_MERGE_COMMIT=0bf01d27afb0cd7df35b1506096a0524db15654d
PR6=MERGED
PR7=MERGED
EXACT_SOURCE_COMMIT_REGRESSION=PENDING_PROVENANCE_AND_RERUN
GITHUB_CI_CHECKS=NONE_RETURNED
RELEASE_CERTIFICATION=NOT_CLAIMED
```
