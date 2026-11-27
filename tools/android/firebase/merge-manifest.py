#!/usr/bin/env python3
"""Merge the libraries' manifests into the app's, as Gradle's manifest merger does
for the parts Firebase uses (docs/push-notifications.md).

    python3 -I merge-manifest.py <app manifest> <application id> <library manifests...>

The app's manifest is rewritten in place. From each library it takes the
<uses-permission> lines and the components inside <application> (activity,
service, receiver, provider, meta-data), with ${applicationId} filled in. A
component that two libraries declare (Firebase's ComponentDiscoveryService) is
one component holding the meta-data of both. `tools:` attributes are dropped.
<application> attributes of a library are not taken: the app's own stand.
"""
import sys
import xml.etree.ElementTree as ElementTree

android = "http://schemas.android.com/apk/res/android"
tools = "http://schemas.android.com/tools"
ElementTree.register_namespace("android", android)
nameKey = f"{{{android}}}name"


def cleaned(element, applicationId):
    for key in list(element.attrib):
        if key.startswith(f"{{{tools}}}"):
            del element.attrib[key]
        else:
            element.attrib[key] = element.attrib[key].replace("${applicationId}", applicationId)
    for child in element:
        cleaned(child, applicationId)
    return element


def childKey(element):
    return (element.tag, element.attrib.get(nameKey), ElementTree.tostring(element) if nameKey not in element.attrib else None)


def main():
    appPath, applicationId = sys.argv[1], sys.argv[2]
    tree = ElementTree.parse(appPath)
    manifest = tree.getroot()
    application = manifest.find("application")
    permissions = {element.attrib.get(nameKey) for element in manifest.findall("uses-permission")}
    components = {(element.tag, element.attrib.get(nameKey)): element for element in application}
    for libraryPath in sys.argv[3:]:
        library = cleaned(ElementTree.parse(libraryPath).getroot(), applicationId)
        for permission in library.findall("uses-permission"):
            if permission.attrib.get(nameKey) not in permissions:
                permissions.add(permission.attrib.get(nameKey))
                manifest.insert(list(manifest).index(application), permission)
        libraryApplication = library.find("application")
        if libraryApplication is None:
            continue
        for component in libraryApplication:
            key = (component.tag, component.attrib.get(nameKey))
            existing = components.get(key)
            if existing is None:
                components[key] = component
                application.append(component)
                continue
            for attribute, value in component.attrib.items():
                existing.attrib.setdefault(attribute, value)
            present = {childKey(child) for child in existing}
            for child in component:
                if childKey(child) not in present:
                    existing.append(child)
    ElementTree.indent(tree, space="  ")
    tree.write(appPath, encoding="utf-8", xml_declaration=True)


main()
