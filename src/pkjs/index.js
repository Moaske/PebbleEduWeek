/* =============================================================
   EduWeek – PebbleKit JS layer
   Fetches the school-week CSV (ISO week, Period, Edu week, Info),
   derives the Monday date of every ISO week, drops past weeks and
   sends one packed string to the watch:

     monday|isoWeek|period|edu|info\n  (one line per week)

   monday = days since 1970-01-01 (the watch does its own date maths)
   ============================================================= */

var Clay        = require('@rebble/clay');
var clayConfig  = require('./config');
var messageKeys = require('message_keys');
var clay        = new Clay(clayConfig, null, { autoHandleEvents: false });

/* Limits must match the buffers in main.c */
var MAX_WEEKS  = 60;   /* MAX_WEEKS          */
var MAX_PERIOD = 5;    /* WeekItem.period[6] */
var MAX_EDU    = 15;   /* WeekItem.edu[16]   */
var MAX_INFO   = 31;   /* WeekItem.info[32]  */
var TIMEOUT_MS = 15000;
var DAY_MS     = 86400000;

function log(msg) { console.log('[EduWeek] ' + msg); }

/* ----------------------------------------------------------
   Settings
---------------------------------------------------------- */
function getCsvUrl() {
  try {
    var s = JSON.parse(localStorage.getItem('clay-settings')) || {};
    return (s.CSV_URL || '').trim();
  } catch (e) {
    return '';
  }
}

/* github.com/<user>/<repo>/blob/<branch>/<path> -> raw.githubusercontent.com/... */
function toRawUrl(url) {
  var m = url.match(/^https?:\/\/github\.com\/([^\/]+)\/([^\/]+)\/(?:blob|raw)\/(.+)$/i);
  if (m) url = 'https://raw.githubusercontent.com/' + m[1] + '/' + m[2] + '/' + m[3];
  /* raw.githubusercontent.com caches ~5 min; a query string gets a fresh copy */
  if (/^https:\/\/raw\.githubusercontent\.com\//i.test(url)) {
    url += (url.indexOf('?') < 0 ? '?' : '&') + 't=' + Date.now();
  }
  return url;
}

/* ----------------------------------------------------------
   CSV parsing (quote-aware, BOM + ; or , delimiter)
---------------------------------------------------------- */
function parseCsv(text) {
  text = text.replace(/^\uFEFF/, '');
  var firstLine = text.split(/\r?\n/)[0] || '';
  var sep = firstLine.split(';').length > firstLine.split(',').length ? ';' : ',';

  var rows = [], row = [], field = '', inQuotes = false;
  for (var i = 0; i < text.length; i++) {
    var c = text.charAt(i);
    if (inQuotes) {
      if (c === '"') {
        if (text.charAt(i + 1) === '"') { field += '"'; i++; }
        else inQuotes = false;
      } else {
        field += c;
      }
    } else if (c === '"') {
      inQuotes = true;
    } else if (c === sep) {
      row.push(field); field = '';
    } else if (c === '\n' || c === '\r') {
      if (c === '\r' && text.charAt(i + 1) === '\n') i++;
      row.push(field); rows.push(row); row = []; field = '';
    } else {
      field += c;
    }
  }
  if (field !== '' || row.length) { row.push(field); rows.push(row); }

  return rows.filter(function (r) { return r.join('').trim() !== ''; });
}

function findColumn(headers, names, fallback) {
  for (var i = 0; i < headers.length; i++) {
    var h = headers[i].trim().toLowerCase();
    if (names.indexOf(h) >= 0) return i;
  }
  return fallback;
}

/* ----------------------------------------------------------
   ISO week date helpers (all in whole days since epoch, UTC)
---------------------------------------------------------- */
function week1Monday(year) {
  var jan4 = Date.UTC(year, 0, 4) / DAY_MS;
  var dow  = (new Date(jan4 * DAY_MS).getUTCDay() + 6) % 7;  /* Mon = 0 */
  return jan4 - dow;
}

function weeksInYear(year) {
  return (week1Monday(year + 1) - week1Monday(year)) / 7;    /* 52 or 53 */
}

function todayDays() {
  var n = new Date();
  return Date.UTC(n.getFullYear(), n.getMonth(), n.getDate()) / DAY_MS;
}

function isoYearOf(days) {
  var y = new Date(days * DAY_MS).getUTCFullYear();
  if (days >= week1Monday(y + 1)) return y + 1;
  if (days < week1Monday(y)) return y - 1;
  return y;
}

/* The CSV has no year column: rows run in school-year order (34..52/53, 1..33),
   so the year goes up by one each time the week number drops. */
function assignMondays(rows, baseYear) {
  var out = [], yearOffset = 0;
  for (var i = 0; i < rows.length; i++) {
    if (i > 0 && rows[i].week < rows[i - 1].week) yearOffset++;
    var year = baseYear + yearOffset;
    if (rows[i].week > weeksInYear(year)) {
      log('Skipping week ' + rows[i].week + ': ' + year + ' has only 52 ISO weeks');
      continue;
    }
    out.push({
      monday: week1Monday(year) + (rows[i].week - 1) * 7,
      week:   rows[i].week,
      period: rows[i].period,
      edu:    rows[i].edu,
      info:   rows[i].info
    });
  }
  return out;
}

function datedWeeks(rows) {
  var today   = todayDays();
  var isoYear = isoYearOf(today);
  var bases   = [isoYear, isoYear - 1];

  /* Prefer the base year whose school year contains today */
  for (var b = 0; b < bases.length; b++) {
    var list = assignMondays(rows, bases[b]);
    if (list.length && list[0].monday <= today &&
        today < list[list.length - 1].monday + 7) {
      return list;
    }
  }
  /* Otherwise (file starts in the future, or is outdated):
     take the base year whose first week lies closest to today */
  var best = null, bestDist = Infinity;
  for (var c = 0; c < bases.length; c++) {
    var cand = assignMondays(rows, bases[c]);
    var dist = cand.length ? Math.abs(cand[0].monday - today) : Infinity;
    if (dist < bestDist) { best = cand; bestDist = dist; }
  }
  return best || [];
}

/* ----------------------------------------------------------
   Build payload
---------------------------------------------------------- */
function clean(s, max) {
  s = String(s || '').replace(/[|\r\n]+/g, ' ').replace(/\s+/g, ' ').trim();
  return s.length > max ? s.substring(0, max) : s;
}

function buildPayload(csvText) {
  var table = parseCsv(csvText);
  if (table.length < 2) throw new Error('CSV is empty');

  var head   = table[0];
  var cWeek  = findColumn(head, ['iso week', 'isoweek', 'week'], 0);
  var cPer   = findColumn(head, ['period', 'periode'], 1);
  var cEdu   = findColumn(head, ['edu week', 'eduweek', 'edu'], 2);
  var cInfo  = findColumn(head, ['info'], 3);

  var rows = [];
  for (var i = 1; i < table.length; i++) {
    var r = table[i];
    var w = parseInt(r[cWeek], 10);
    if (!(w >= 1 && w <= 53)) continue;
    rows.push({
      week:   w,
      period: clean(r[cPer], MAX_PERIOD),
      edu:    clean(r[cEdu], MAX_EDU),
      info:   clean(r[cInfo], MAX_INFO)
    });
  }
  if (!rows.length) throw new Error('No week rows found');

  var today = todayDays();
  var weeks = datedWeeks(rows).filter(function (x) {
    return x.monday + 7 > today;            /* drop past weeks */
  }).slice(0, MAX_WEEKS);

  log('Parsed ' + rows.length + ' rows, sending ' + weeks.length + ' weeks');
  return weeks.map(function (x) {
    return [x.monday, x.week, x.period, x.edu, x.info].join('|');
  }).join('\n');
}

/* ----------------------------------------------------------
   Watch messaging
---------------------------------------------------------- */
function send(key, value) {
  var msg = {};
  msg[messageKeys[key]] = value;
  Pebble.sendAppMessage(msg,
    function ()  { log('Sent ' + key); },
    function (e) { log('Send ' + key + ' failed: ' + JSON.stringify(e)); });
}

function fetchWeeks() {
  var url = getCsvUrl();
  if (!url) {
    send('STATUS', 'Set the CSV URL\nin the app settings');
    return;
  }

  var req = new XMLHttpRequest();
  req.open('GET', toRawUrl(url), true);
  req.timeout = TIMEOUT_MS;
  req.onload = function () {
    if (req.status !== 200) {
      send('STATUS', 'Download failed\n(HTTP ' + req.status + ')');
      return;
    }
    try {
      var payload = buildPayload(req.responseText);
      if (!payload) {
        send('STATUS', 'No upcoming weeks\nin the CSV');
        return;
      }
      send('WEEKS', payload);
    } catch (e) {
      log('Parse error: ' + e.message);
      send('STATUS', 'Could not read\nthe CSV file');
    }
  };
  req.onerror   = function () { send('STATUS', 'No connection'); };
  req.ontimeout = function () { send('STATUS', 'Download timed out'); };
  req.send();
}

/* ----------------------------------------------------------
   Events
---------------------------------------------------------- */
Pebble.addEventListener('ready', function () {
  log('JS ready');
  fetchWeeks();
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  clay.getSettings(e.response);   /* stores into localStorage 'clay-settings' */
  fetchWeeks();
});
