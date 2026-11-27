#!/usr/bin/env python3
"""The app's google-services.json as the string resources Firebase reads at start
(FirebaseInitProvider), as Google's Gradle plugin writes them (docs/push-notifications.md).

    python3 -I google-services-values.py <google-services.json> <application id> <values.xml>

The JSON is the app's, downloaded from its Firebase project; it stays outside the
repository. Only the client whose package name is the application id is used.
"""
import json
import sys
from xml.sax.saxutils import escape

source, applicationId, target = sys.argv[1], sys.argv[2], sys.argv[3]
with open(source) as file:
    config = json.load(file)
project = config.get("project_info", {})
client = None
for candidate in config.get("client", []):
    if candidate.get("client_info", {}).get("android_client_info", {}).get("package_name") == applicationId:
        client = candidate
if client is None:
    raise SystemExit(f"error: {source} has no Android app with the package {applicationId}")
values = {
    "google_app_id": client["client_info"]["mobilesdk_app_id"],
    "gcm_defaultSenderId": project.get("project_number", ""),
    "project_id": project.get("project_id", ""),
    "google_storage_bucket": project.get("storage_bucket", ""),
    "google_api_key": (client.get("api_key") or [{}])[0].get("current_key", ""),
}
values["google_crash_reporting_api_key"] = values["google_api_key"]
lines = ['<?xml version="1.0" encoding="utf-8"?>', "<resources>"]
for name, value in values.items():
    if value:
        lines.append(f'  <string name="{name}" translatable="false">{escape(value)}</string>')
lines.append("</resources>")
with open(target, "w") as file:
    file.write("\n".join(lines) + "\n")
