#include <algorithm>
#include <cctype>
#include <vector>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>  // already pulls in all GFXFF font headers via gfxfont.h
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#if __has_include("Secrets.h")
#include "Secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef WIFI_SSID_2
#define WIFI_SSID_2 ""
#endif

#ifndef WIFI_PASSWORD_2
#define WIFI_PASSWORD_2 ""
#endif

#ifndef TODOIST_API_TOKEN
#define TODOIST_API_TOKEN ""
#endif

#ifndef TIMEZONE_POSIX
#define TIMEZONE_POSIX "UTC0"
#endif

namespace Pins {
constexpr uint8_t kTftBacklight = 21;
constexpr uint8_t kTouchIrq     = 36;
constexpr uint8_t kTouchCs      = 33;
constexpr uint8_t kTouchClk     = 25;
constexpr uint8_t kTouchMiso    = 39;
constexpr uint8_t kTouchMosi    = 32;
}  // namespace Pins

// tft.invertDisplay(true) in configureDisplay() corrects the panel polarity on
// this CYD revision.  After that, standard RGB565 applies throughout:
//   ((r>>3)<<11) | ((g>>2)<<5) | (b>>3)
namespace UiColors {
constexpr uint16_t kBackground    = 0x1082;  // RGB( 16, 16, 16) near-black
constexpr uint16_t kHeader        = 0x18C3;  // RGB( 24, 24, 24) elevated surface
constexpr uint16_t kCard          = 0x2945;  // RGB( 40, 40, 40) card surface
constexpr uint16_t kCardBorder    = 0x3A08;  // RGB( 56, 64, 64) soft border
constexpr uint16_t kDivider       = 0x39C7;  // RGB( 56, 56, 56) subtle separator
constexpr uint16_t kTextPrimary   = 0xF79E;  // RGB(240,240,240) near-white
constexpr uint16_t kTextSecondary = 0x94F5;  // RGB(144,156,168) muted cool grey
constexpr uint16_t kTextTertiary  = 0x73AE;  // RGB(112,116,120) softer grey
constexpr uint16_t kAccent        = 0x3DFF;  // RGB( 56,188,248) sky blue
constexpr uint16_t kOverdue       = 0xEA28;  // RGB(232, 68, 64) red
constexpr uint16_t kDueToday      = 0xF4E1;  // RGB(240,156,  8) amber
constexpr uint16_t kDueSoon       = 0x262B;  // RGB( 32,196, 88) green
constexpr uint16_t kMuted         = 0x6B90;  // RGB(104,112,128) cool grey
constexpr uint16_t kPriority4     = 0xEA28;  // RGB(232, 68, 64) red    (urgent)
constexpr uint16_t kPriority3     = 0xFB82;  // RGB(248,112, 16) orange  (high)
constexpr uint16_t kPriority2     = 0x3DFF;  // RGB( 56,188,248) blue    (medium)
}  // namespace UiColors

constexpr char kTodoistSyncUrl[] = "https://api.todoist.com/api/v1/sync";
constexpr char kTodoistProjectsBody[] =
    "sync_token=*&resource_types=%5B%22projects%22%5D";
constexpr char kTodoistItemsBody[] =
    "sync_token=*&resource_types=%5B%22items%22%5D";
constexpr char kAppTitle[] = "Tim's Tasks";
constexpr size_t kMaxTasks = 5;
constexpr uint32_t kAutoRefreshMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kRefreshRetryBackoffMs = 60UL * 1000UL;
constexpr uint32_t kTouchDebounceMs = 500;
constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kNtpTimeoutMs = 15000;
constexpr uint32_t kHttpTimeoutMs = 15000;
constexpr time_t kValidEpochThreshold = 1700000000;
constexpr uint8_t kDisplayRotation = 0;  // Portrait, USB-C at bottom

constexpr int kHeaderH = 24;   // compact header for title + status
constexpr int kSubtitleH = 0;
constexpr int kFooterH = 0;

struct ProjectRecord {
  String id;
  String name;
};

struct TaskRecord {
  String content;
  String projectId;
  String projectName;
  String dueLabel;
  String dueDateRaw;
  int priority = 1;
  int childOrder = 0;
  bool hasDue = false;
  bool isOverdue = false;
  bool isToday = false;
  time_t dueEpoch = LONG_MAX;
};

struct FetchOutcome {
  std::vector<TaskRecord> tasks;
  String errorMessage;
  bool ok = false;
};

WiFiMulti g_wifiMulti;
TFT_eSPI tft;
TFT_eSprite g_cardSprite(&tft);

XPT2046_Touchscreen g_touch(Pins::kTouchCs, Pins::kTouchIrq);

std::vector<TaskRecord> g_tasks;
String g_lastHeaderClock = "";
bool g_touchArmed = true;
bool g_timeConfigured = false;
bool g_dashboardVisible = false;
uint32_t g_lastRefreshAt = 0;
uint32_t g_lastRefreshAttemptAt = 0;
uint32_t g_lastTouchAt = 0;
int g_cardSpriteWidth = 0;
int g_cardSpriteHeight = 0;
FetchOutcome g_fetchOutcome;

bool isConfigured() {
  return strlen(WIFI_SSID) > 0 && strlen(WIFI_PASSWORD) > 0 &&
         strlen(TODOIST_API_TOKEN) > 0;
}

String jsonToString(JsonVariantConst value) {
  if (value.isNull()) {
    return "";
  }
  if (value.is<const char*>()) {
    return String(value.as<const char*>());
  }

  String out;
  serializeJson(value, out);

  if (out.length() >= 2 && out[0] == '"' && out[out.length() - 1] == '"') {
    out.remove(out.length() - 1);
    out.remove(0, 1);
  }

  return out;
}

String ellipsize(const String& input, size_t maxChars) {
  if (input.length() <= maxChars) {
    return input;
  }

  if (maxChars < 4) {
    return input.substring(0, maxChars);
  }

  return input.substring(0, maxChars - 3) + "...";
}

String lookupProjectName(const std::vector<ProjectRecord>& projects,
                         const String& id) {
  for (const auto& project : projects) {
    if (project.id == id) {
      return project.name;
    }
  }
  return "Inbox";
}

bool beginTodoistSyncRequest(WiFiClientSecure& client,
                             HTTPClient& http,
                             const char* body,
                             String& errorMessage) {
  client.setInsecure();
  client.setTimeout(kHttpTimeoutMs / 1000);
  client.setHandshakeTimeout(kHttpTimeoutMs / 1000);

  http.useHTTP10(true);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, kTodoistSyncUrl)) {
    errorMessage = "Unable to open HTTPS connection";
    return false;
  }

  http.addHeader("Authorization", "Bearer " + String(TODOIST_API_TOKEN));
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  const int statusCode = http.POST(body);
  if (statusCode != HTTP_CODE_OK) {
    errorMessage = "Todoist returned HTTP " + String(statusCode) + " " +
                   http.errorToString(statusCode);
    http.end();
    return false;
  }

  return true;
}

bool moveStreamToArray(Stream& stream, const char* key, String& errorMessage) {
  if (!stream.find(const_cast<char*>(key))) {
    errorMessage = "Todoist response did not contain expected data";
    return false;
  }

  if (!stream.find(const_cast<char*>("["))) {
    errorMessage = "Todoist response array was malformed";
    return false;
  }

  return true;
}

bool prepareNextArrayValue(Stream& stream,
                           bool& reachedEnd,
                           String& errorMessage) {
  reachedEnd = false;

  while (true) {
    const int nextChar = stream.peek();
    if (nextChar < 0) {
      errorMessage = "Unexpected end of Todoist response";
      return false;
    }

    if (std::isspace(static_cast<unsigned char>(nextChar)) || nextChar == ',') {
      stream.read();
      continue;
    }

    if (nextChar == ']') {
      stream.read();
      reachedEnd = true;
      return true;
    }

    return true;
  }
}

time_t mktimeUtc(const struct tm& value) {
  const String originalTz = String(getenv("TZ") ? getenv("TZ") : "");
  struct tm utcValue = value;

  setenv("TZ", "UTC0", 1);
  tzset();
  const time_t epoch = mktime(&utcValue);
  setenv("TZ", originalTz.c_str(), 1);
  tzset();

  return epoch;
}

bool parseUtcOffset(const String& offset, int32_t& secondsOut) {
  if (offset.length() != 6 || (offset[0] != '+' && offset[0] != '-') ||
      offset[3] != ':') {
    return false;
  }

  for (int i = 1; i < offset.length(); ++i) {
    if (i == 3) {
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(offset[i]))) {
      return false;
    }
  }

  const int hours = offset.substring(1, 3).toInt();
  const int minutes = offset.substring(4, 6).toInt();
  if (hours > 23 || minutes > 59) {
    return false;
  }

  secondsOut = (hours * 60 + minutes) * 60;
  if (offset[0] == '-') {
    secondsOut = -secondsOut;
  }

  return true;
}

bool parseDueDate(const String& dueDate, time_t& epochOut, bool& allDayOut) {
  if (dueDate.isEmpty()) {
    return false;
  }

  struct tm tmValue = {};
  String normalized = dueDate;
  allDayOut = dueDate.length() == 10;

  if (!allDayOut) {
    normalized = dueDate.substring(0, 19);
  }

  const char* format = allDayOut ? "%Y-%m-%d" : "%Y-%m-%dT%H:%M:%S";
  char buffer[24] = {0};
  normalized.toCharArray(buffer, sizeof(buffer));

  if (strptime(buffer, format, &tmValue) == nullptr) {
    return false;
  }

  if (!allDayOut) {
    if (dueDate.endsWith("Z")) {
      epochOut = mktimeUtc(tmValue);
      return epochOut > 0;
    }

    int32_t offsetSeconds = 0;
    const int plusIndex = dueDate.indexOf('+', 19);
    const int minusIndex = dueDate.lastIndexOf('-');
    const int offsetIndex =
        plusIndex >= 19 ? plusIndex : (minusIndex >= 19 ? minusIndex : -1);
    if (offsetIndex >= 19 &&
        parseUtcOffset(dueDate.substring(offsetIndex), offsetSeconds)) {
      epochOut = mktimeUtc(tmValue) - offsetSeconds;
      return epochOut > 0;
    }
  }

  epochOut = mktime(&tmValue);
  return epochOut > 0;
}

void classifyDue(TaskRecord& task) {
  if (!task.hasDue) {
    return;
  }

  bool allDay = false;
  time_t dueEpoch = 0;
  if (!parseDueDate(task.dueDateRaw, dueEpoch, allDay)) {
    return;
  }

  task.dueEpoch = dueEpoch;

  const time_t now = time(nullptr);
  struct tm nowTm = {};
  struct tm dueTm = {};
  localtime_r(&now, &nowTm);
  localtime_r(&dueEpoch, &dueTm);

  if (allDay) {
    const bool sameDay =
        nowTm.tm_year == dueTm.tm_year && nowTm.tm_yday == dueTm.tm_yday;
    task.isToday = sameDay;
    task.isOverdue = !sameDay && difftime(dueEpoch, now) < 0;
    return;
  }

  task.isToday =
      nowTm.tm_year == dueTm.tm_year && nowTm.tm_yday == dueTm.tm_yday;
  task.isOverdue = difftime(dueEpoch, now) < 0;
}

String formatClock(time_t value, const char* format) {
  if (value <= 0) {
    return "Unknown";
  }

  struct tm tmValue = {};
  localtime_r(&value, &tmValue);

  char buffer[32] = {0};
  strftime(buffer, sizeof(buffer), format, &tmValue);
  return String(buffer);
}

String currentHeaderClockText() {
  if (!g_timeConfigured || time(nullptr) < kValidEpochThreshold) {
    return "--:--";
  }
  return formatClock(time(nullptr), "%H:%M");
}

uint16_t dueColor(const TaskRecord& task) {
  if (!task.hasDue) {
    return UiColors::kMuted;
  }
  if (task.isOverdue) {
    return UiColors::kOverdue;
  }
  if (task.isToday) {
    return UiColors::kDueToday;
  }
  return UiColors::kDueSoon;
}

uint16_t priorityColor(int priority) {
  switch (priority) {
    case 4: return UiColors::kPriority4;
    case 3: return UiColors::kPriority3;
    case 2: return UiColors::kPriority2;
    default: return UiColors::kMuted;
  }
}

bool taskComesBefore(const TaskRecord& left, const TaskRecord& right) {
  if (left.hasDue != right.hasDue) {
    return left.hasDue;
  }
  if (left.hasDue && right.hasDue && left.dueEpoch != right.dueEpoch) {
    return left.dueEpoch < right.dueEpoch;
  }
  if (left.priority != right.priority) {
    return left.priority > right.priority;
  }
  if (left.childOrder != right.childOrder) {
    return left.childOrder < right.childOrder;
  }
  return left.content < right.content;
}

bool ensureCardSprite(int width, int height) {
  if (g_cardSprite.created() && g_cardSpriteWidth == width &&
      g_cardSpriteHeight == height) {
    return true;
  }

  if (g_cardSprite.created()) {
    g_cardSprite.deleteSprite();
  }

  if (!g_cardSprite.createSprite(width, height)) {
    g_cardSpriteWidth = 0;
    g_cardSpriteHeight = 0;
    return false;
  }

  g_cardSpriteWidth = width;
  g_cardSpriteHeight = height;
  return true;
}

void loadUiSmoothFont(TFT_eSPI& surface) {
  surface.setFreeFont(&FreeSansBold9pt7b);
}

void unloadUiSmoothFont(TFT_eSPI& surface) {
  surface.setFreeFont(nullptr);
}

void drawCenteredStatus(const String& title,
                        const String& line1,
                        const String& line2 = "") {
  g_dashboardVisible = false;
  tft.fillScreen(UiColors::kBackground);
  tft.fillRect(0, 0, tft.width(), kHeaderH, UiColors::kHeader);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(UiColors::kTextPrimary, UiColors::kHeader);
  loadUiSmoothFont(tft);
  tft.setTextColor(UiColors::kTextPrimary, UiColors::kHeader, true);
  tft.drawString(title, tft.width() / 2, kHeaderH / 2);

  tft.setTextColor(UiColors::kTextPrimary, UiColors::kBackground, true);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2);
  unloadUiSmoothFont(tft);

  if (!line2.isEmpty()) {
    tft.setTextColor(UiColors::kTextSecondary, UiColors::kBackground);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 22);
  }

  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
}

void drawHeader() {
  tft.fillRect(0, 0, tft.width(), kHeaderH, UiColors::kBackground);

  tft.setTextDatum(ML_DATUM);
  loadUiSmoothFont(tft);
  tft.setTextColor(UiColors::kTextPrimary, UiColors::kBackground, true);
  tft.drawString(kAppTitle, 12, kHeaderH / 2);
  unloadUiSmoothFont(tft);

  // Right-aligned clock before the Wi-Fi dot.
  const String statusText = currentHeaderClockText();
  tft.setTextColor(UiColors::kMuted, UiColors::kBackground);
  tft.setTextDatum(MR_DATUM);
  tft.setTextFont(2);
  tft.drawString(statusText, tft.width() - 22, kHeaderH / 2);

  // Wifi dot — right edge
  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  const uint16_t dotColor = wifiUp ? UiColors::kDueSoon : UiColors::kOverdue;
  tft.fillCircle(tft.width() - 12, kHeaderH / 2, 4, dotColor);

  tft.setFreeFont(nullptr);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
}

void updateHeaderClockIfNeeded() {
  if (!g_dashboardVisible) {
    return;
  }

  const String clockText = currentHeaderClockText();
  if (clockText == g_lastHeaderClock) {
    return;
  }

  g_lastHeaderClock = clockText;
  drawHeader();
}


void drawDimOverlay() {
  // Fill the content area with near-black to dim task cards behind the spinner.
  tft.fillRect(0, kHeaderH, tft.width(), tft.height() - kHeaderH, 0x0000);
}

// 8 dot positions around a circle of radius 18, starting at 12 o'clock clockwise.
// Pre-computed as integers to avoid float trig at runtime.
void drawSpinner(int cx, int cy, int frame) {
  static const int8_t kDx[8] = {  0, 13, 18, 13,  0, -13, -18, -13 };
  static const int8_t kDy[8] = { -18,-13,  0, 13, 18,  13,   0, -13 };

  tft.fillCircle(cx, cy, 24, UiColors::kCard);
  for (int i = 0; i < 8; ++i) {
    const int    age = (8 + frame - i) % 8;
    uint16_t     col;
    if      (age == 0) col = UiColors::kAccent;
    else if (age <= 2) col = UiColors::kMuted;
    else               col = UiColors::kDivider;
    tft.fillCircle(cx + kDx[i], cy + kDy[i], 3, col);
  }
}

int measureWrappedLines(TFT_eSPI& surface, const String& text, int maxW, int maxLines) {
  int lines = 0, pos = 0;
  const int len = (int)text.length();
  while (pos < len && lines < maxLines) {
    String seg;
    int segEnd = pos;
    while (segEnd <= len) {
      int sp = text.indexOf(' ', segEnd);
      if (sp < 0) sp = len;
      const String cand = seg.isEmpty() ? text.substring(segEnd, sp)
                                        : seg + ' ' + text.substring(segEnd, sp);
      if (surface.textWidth(cand) <= maxW) {
        seg = cand; segEnd = sp + 1;
        if (sp >= len) break;
      } else {
        if (seg.isEmpty()) { seg = text.substring(segEnd, sp); segEnd = sp + 1; }
        break;
      }
    }
    ++lines; pos = segEnd;
  }
  return std::max(1, lines);
}

void drawWrappedTitle(TFT_eSPI& surface, const String& text, int x, int y,
                      int maxW, int lineH, int maxLines) {
  int pos = 0;
  const int len = (int)text.length();
  for (int line = 0; line < maxLines && pos < len; ++line) {
    String seg;
    int segEnd = pos;
    while (segEnd <= len) {
      int sp = text.indexOf(' ', segEnd);
      if (sp < 0) sp = len;
      const String cand = seg.isEmpty() ? text.substring(segEnd, sp)
                                        : seg + ' ' + text.substring(segEnd, sp);
      if (surface.textWidth(cand) <= maxW) {
        seg = cand;
        segEnd = sp + 1;
        if (sp >= len) break;
      } else {
        if (seg.isEmpty()) { seg = text.substring(segEnd, sp); segEnd = sp + 1; }
        break;
      }
    }
    const bool hasMore = segEnd <= len;
    if (line == maxLines - 1 && hasMore) {
      while (!seg.isEmpty() && surface.textWidth(seg + "...") > maxW) {
        const int sp = seg.lastIndexOf(' ');
        seg = sp > 0 ? seg.substring(0, sp) : seg.substring(0, (int)seg.length() - 1);
      }
      seg += "...";
    }
    surface.drawString(seg, x, y + line * lineH);
    pos = segEnd;
  }
}

void drawTaskCard(const TaskRecord& task, int top, int cardH) {
  const int left  = 8;
  const int width = tft.width() - 16;

  if (!ensureCardSprite(width, cardH)) return;

  const int textX = 17;
  const uint16_t stripe = priorityColor(task.priority);

  g_cardSprite.fillSprite(UiColors::kBackground);
  g_cardSprite.fillRoundRect(0, 0, width, cardH, 8, UiColors::kCard);
  g_cardSprite.drawRoundRect(0, 0, width, cardH, 8, UiColors::kCardBorder);
  g_cardSprite.fillRoundRect(6, 6, 4, cardH - 12, 2, stripe);

  int titleMaxW = width - textX - 6;

  if (task.hasDue) {
    const String dueText = ellipsize(task.dueLabel, 10);
    g_cardSprite.setTextFont(1);
    const int badgeTextWidth = g_cardSprite.textWidth(dueText);
    const int badgeW = badgeTextWidth + 10;
    const int badgeH = 11;
    const int badgeX = width - badgeW - 7;
    const int badgeY = 5;

    g_cardSprite.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 5, dueColor(task));
    g_cardSprite.setTextDatum(MC_DATUM);
    g_cardSprite.setTextColor(UiColors::kTextPrimary, dueColor(task));
    g_cardSprite.drawString(dueText, badgeX + badgeW / 2, badgeY + badgeH / 2 + 1);
    g_cardSprite.setTextDatum(TL_DATUM);
    titleMaxW = badgeX - textX - 4;
  }

  loadUiSmoothFont(g_cardSprite);
  const int lineH    = g_cardSprite.fontHeight();
  const int numLines = measureWrappedLines(g_cardSprite, task.content, titleMaxW, 2);

  // Vertically center the content block (title + gap + subtitle) within the card.
  const int subtitleH = 12;
  const int blockH    = numLines * lineH + 5 + subtitleH;
  const int contentY  = std::max(4, (cardH - blockH) / 2);
  const int subtitleY = contentY + numLines * lineH + 5;

  g_cardSprite.setTextDatum(TL_DATUM);
  g_cardSprite.setTextColor(UiColors::kTextPrimary, UiColors::kCard, true);
  drawWrappedTitle(g_cardSprite, task.content, textX, contentY, titleMaxW, lineH, numLines);
  unloadUiSmoothFont(g_cardSprite);

  g_cardSprite.fillCircle(textX + 1, subtitleY + 4, 2, stripe);
  g_cardSprite.setTextFont(1);
  g_cardSprite.setTextDatum(TL_DATUM);
  g_cardSprite.setTextColor(UiColors::kTextSecondary, UiColors::kCard);
  g_cardSprite.drawString(ellipsize(task.projectName, 26), textX + 8, subtitleY);

  g_cardSprite.setTextFont(1);
  g_cardSprite.setTextDatum(TL_DATUM);
  g_cardSprite.pushSprite(left, top);
}

void renderDashboard() {
  g_dashboardVisible = true;
  g_lastHeaderClock = currentHeaderClockText();
  tft.fillScreen(UiColors::kBackground);
  drawHeader();

  if (g_tasks.empty()) {
    tft.setTextDatum(MC_DATUM);
    loadUiSmoothFont(tft);
    tft.setTextColor(UiColors::kTextPrimary, UiColors::kBackground, true);
    tft.drawString("No active tasks", tft.width() / 2, tft.height() / 2);
    unloadUiSmoothFont(tft);
    tft.setTextColor(UiColors::kTextSecondary, UiColors::kBackground);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("Todoist returned an empty list.",
                   tft.width() / 2, tft.height() / 2 + 22);
    tft.setTextFont(1);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  const int cardsTop    = kHeaderH;
  const int cardsBottom = tft.height();
  const int taskCount   = (int)g_tasks.size();
  const int cardSlot    = (cardsBottom - cardsTop) / taskCount;

  int top = cardsTop;
  for (size_t i = 0; i < g_tasks.size(); ++i) {
    const bool isLast = (i == g_tasks.size() - 1);
    const int  cardH  = isLast ? (cardsBottom - top) : (cardSlot - 1);
    drawTaskCard(g_tasks[i], top, cardH);
    top += cardSlot;
  }
}


StaticJsonDocument<96> buildProjectFilter() {
  StaticJsonDocument<96> filter;
  filter["id"] = true;
  filter["name"] = true;
  return filter;
}

StaticJsonDocument<256> buildTaskFilter() {
  StaticJsonDocument<256> filter;
  filter["content"] = true;
  filter["project_id"] = true;
  filter["priority"] = true;
  filter["child_order"] = true;
  filter["checked"] = true;
  filter["is_deleted"] = true;
  filter["due"]["date"] = true;
  filter["due"]["string"] = true;
  return filter;
}

bool fetchProjects(std::vector<ProjectRecord>& projects, String& errorMessage) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginTodoistSyncRequest(client, http, kTodoistProjectsBody,
                               errorMessage)) {
    return false;
  }

  Stream& stream = http.getStream();
  if (!moveStreamToArray(stream, "\"projects\"", errorMessage)) {
    http.end();
    return false;
  }

  StaticJsonDocument<96> filter = buildProjectFilter();
  DynamicJsonDocument doc(1024);

  while (true) {
    bool reachedEnd = false;
    if (!prepareNextArrayValue(stream, reachedEnd, errorMessage)) {
      http.end();
      return false;
    }
    if (reachedEnd) {
      break;
    }

    doc.clear();
    const DeserializationError jsonError =
        deserializeJson(doc, stream, DeserializationOption::Filter(filter));
    if (jsonError) {
      errorMessage = "Project parse failed: " + String(jsonError.c_str());
      http.end();
      return false;
    }
    if (doc.overflowed()) {
      errorMessage = "Project data exceeded parsing buffer";
      http.end();
      return false;
    }

    ProjectRecord record;
    record.id = jsonToString(doc["id"]);
    record.name = jsonToString(doc["name"]);
    projects.push_back(record);
  }

  http.end();
  return true;
}

bool fetchItems(std::vector<TaskRecord>& tasks, String& errorMessage) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginTodoistSyncRequest(client, http, kTodoistItemsBody, errorMessage)) {
    return false;
  }

  Stream& stream = http.getStream();
  if (!moveStreamToArray(stream, "\"items\"", errorMessage)) {
    http.end();
    return false;
  }

  StaticJsonDocument<256> filter = buildTaskFilter();
  DynamicJsonDocument doc(4096);

  while (true) {
    bool reachedEnd = false;
    if (!prepareNextArrayValue(stream, reachedEnd, errorMessage)) {
      http.end();
      return false;
    }
    if (reachedEnd) {
      break;
    }

    doc.clear();
    const DeserializationError jsonError =
        deserializeJson(doc, stream, DeserializationOption::Filter(filter));
    if (jsonError) {
      errorMessage = "Task parse failed: " + String(jsonError.c_str());
      http.end();
      return false;
    }
    if (doc.overflowed()) {
      errorMessage = "Task data exceeded parsing buffer";
      http.end();
      return false;
    }

    if ((doc["checked"] | false) || (doc["is_deleted"] | false)) {
      continue;
    }

    JsonObjectConst item = doc.as<JsonObjectConst>();
    TaskRecord task;
    task.content = jsonToString(item["content"]);
    task.priority = item["priority"] | 1;
    task.childOrder = item["child_order"] | 0;
    task.projectId = jsonToString(item["project_id"]);

    JsonVariantConst due = item["due"];
    if (!due.isNull()) {
      task.hasDue = true;
      task.dueLabel = jsonToString(due["string"]);
      task.dueDateRaw = jsonToString(due["date"]);
      classifyDue(task);
    }

    auto insertPos =
        std::find_if(tasks.begin(), tasks.end(),
                     [&](const TaskRecord& existing) {
                       return taskComesBefore(task, existing);
                     });
    tasks.insert(insertPos, std::move(task));

    if (tasks.size() > kMaxTasks) {
      tasks.pop_back();
    }
  }

  http.end();
  return true;
}

bool fetchTasks(FetchOutcome& outcome) {
  std::vector<ProjectRecord> projects;
  if (!fetchProjects(projects, outcome.errorMessage)) {
    return false;
  }

  std::vector<TaskRecord> tasks;
  if (!fetchItems(tasks, outcome.errorMessage)) {
    return false;
  }

  for (TaskRecord& task : tasks) {
    task.projectName = lookupProjectName(projects, task.projectId);
  }

  outcome.tasks = std::move(tasks);
  outcome.ok = true;
  return true;
}

bool ensureWiFiConnected(int cx, int cy, int& frame) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  const uint32_t startedAt = millis();
  while (g_wifiMulti.run() != WL_CONNECTED && millis() - startedAt < kWifiTimeoutMs) {
    drawSpinner(cx, cy, frame++);
    delay(100);
  }

  return WiFi.status() == WL_CONNECTED;
}

bool ensureTimeSynced(int cx, int cy, int& frame) {
  if (g_timeConfigured && time(nullptr) >= kValidEpochThreshold) {
    return true;
  }

  configTzTime(TIMEZONE_POSIX, "pool.ntp.org", "time.nist.gov");
  g_timeConfigured = true;

  const uint32_t startedAt = millis();
  while (time(nullptr) < kValidEpochThreshold &&
         millis() - startedAt < kNtpTimeoutMs) {
    drawSpinner(cx, cy, frame++);
    delay(100);
  }

  return time(nullptr) >= kValidEpochThreshold;
}

// Runs fetchTasks() on core 0 so the spinner can animate on core 1 unblocked.
void fetchTasksTask(void* param) {
  TaskHandle_t notifyTarget = static_cast<TaskHandle_t>(param);
  FetchOutcome outcome;
  fetchTasks(outcome);
  g_fetchOutcome = std::move(outcome);
  xTaskNotifyGive(notifyTarget);
  vTaskDelete(nullptr);
}

bool refreshTasks() {
  g_lastRefreshAttemptAt = millis();

  const int cx    = tft.width() / 2;
  const int cy    = tft.height() / 2;
  int       frame = 0;

  drawDimOverlay();

  if (!ensureWiFiConnected(cx, cy, frame)) {
    drawCenteredStatus(kAppTitle, "Wi-Fi failed",
                       "Check include/Secrets.h and network coverage.");
    return false;
  }

  if (!ensureTimeSynced(cx, cy, frame)) {
    drawCenteredStatus(kAppTitle, "Time sync failed",
                       "NTP did not respond before timeout.");
    return false;
  }

  // Run the blocking HTTP fetch on core 0; animate here on core 1 while it runs.
  g_fetchOutcome = FetchOutcome{};
  const BaseType_t taskCreated = xTaskCreatePinnedToCore(
      fetchTasksTask, "fetch", 8192, xTaskGetCurrentTaskHandle(), 1, nullptr, 0);
  if (taskCreated != pdPASS) {
    drawCenteredStatus(kAppTitle, "Sync failed",
                       "Unable to start background fetch task.");
    return false;
  }

  while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) {
    drawSpinner(cx, cy, frame++);
  }

  if (!g_fetchOutcome.ok) {
    drawCenteredStatus(kAppTitle, "Sync failed", g_fetchOutcome.errorMessage);
    return false;
  }

  g_tasks = std::move(g_fetchOutcome.tasks);
  g_lastRefreshAt = millis();
  renderDashboard();
  return true;
}

void configureDisplay() {
  pinMode(Pins::kTftBacklight, OUTPUT);
  digitalWrite(Pins::kTftBacklight, HIGH);

  tft.init();
  tft.invertDisplay(true);  // CYD panel polarity is inverted; this corrects it
  tft.setRotation(kDisplayRotation);
  tft.fillScreen(UiColors::kBackground);
  tft.setTextWrap(false);

  Serial.printf("Display rotation=%u, size=%dx%d\n", kDisplayRotation,
                tft.width(), tft.height());
}

void configureTouch() {
  // TFT_eSPI is compiled to use HSPI on this CYD, so the default SPI instance
  // remains available for the XPT2046 on its own VSPI pin mapping.
  SPI.begin(Pins::kTouchClk, Pins::kTouchMiso, Pins::kTouchMosi, Pins::kTouchCs);
  g_touch.begin();
  g_touch.setRotation(kDisplayRotation);
}

void configureWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (strlen(WIFI_SSID) > 0)
    g_wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  if (strlen(WIFI_SSID_2) > 0)
    g_wifiMulti.addAP(WIFI_SSID_2, WIFI_PASSWORD_2);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  configureDisplay();
  configureTouch();
  configureWiFi();

  if (!isConfigured()) {
    drawCenteredStatus(kAppTitle, "Missing Secrets.h",
                       "Copy include/Secrets.h.example to Secrets.h");
    return;
  }

  refreshTasks();
}

void loop() {
  if (!isConfigured()) {
    delay(250);
    return;
  }

  updateHeaderClockIfNeeded();

  const bool touchDown = g_touch.tirqTouched() && g_touch.touched();
  if (touchDown && g_touchArmed && millis() - g_lastTouchAt > kTouchDebounceMs) {
    g_touchArmed = false;
    g_lastTouchAt = millis();
    refreshTasks();
  } else if (!touchDown) {
    g_touchArmed = true;
  }

  const bool refreshDue = millis() - g_lastRefreshAt > kAutoRefreshMs;
  const bool retryAllowed =
      g_lastRefreshAttemptAt == 0 ||
      millis() - g_lastRefreshAttemptAt >= kRefreshRetryBackoffMs;
  if (refreshDue && retryAllowed) {
    refreshTasks();
  }

  delay(50);
}
