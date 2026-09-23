module.exports = [
  {
    "type": "heading",
    "defaultValue": "EduWeek Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Data source"
      },
      {
        "type": "input",
        "messageKey": "CSV_URL",
        "defaultValue": "",
        "label": "CSV file URL",
        "description": "Link to the week CSV (columns: ISO week, Period, Edu week, Info). " +
                       "A normal github.com/&hellip;/blob/&hellip; link is fine: it is converted " +
                       "to the raw file link automatically.",
        "attributes": {
          "type": "url",
          "placeholder": "https://github.com/Moaske/<repo>/blob/main/weeknumbers.csv",
          "autocapitalize": "off",
          "autocorrect": "off",
          "spellcheck": "false"
        }
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
