# Writes the storeKit purchase test's Xcode project (../run.sh): an app target and a unit-test
# target hosted by it, ad-hoc signed, for macOS 13 or later.
#
#   python3 genProject.py <folder> <bundle id> <app sources> <test sources> <test resources> \
#     <extra app build settings>
# Lists are comma-separated file names inside <folder>.
import os, sys, hashlib
root = sys.argv[1]; name = "StoreHost"; bundle = sys.argv[2]
app_sources = sys.argv[3].split(",")          # files relative to root
test_sources = sys.argv[4].split(",")
test_resources = sys.argv[5].split(",") if len(sys.argv) > 5 and sys.argv[5] else []
extra_app_settings = sys.argv[6] if len(sys.argv) > 6 else ""
ids = {}
def oid(key):
    if key not in ids: ids[key] = hashlib.md5(key.encode()).hexdigest()[:24].upper()
    return ids[key]
def ftype(f):
    return {"swift":"sourcecode.swift","c":"sourcecode.c.c","h":"sourcecode.c.h","storekit":"text","m":"sourcecode.c.objc","a":"archive.ar","dylib":"compiled.mach-o.dylib"}.get(f.rsplit(".",1)[-1],"text")
files = app_sources + test_sources + test_resources
o = []
o.append("// !$*UTF8*$!\n{\n archiveVersion = 1;\n classes = {};\n objectVersion = 56;\n objects = {\n")
for f in files:
    o.append(f' {oid("ref"+f)} = {{isa = PBXFileReference; lastKnownFileType = {ftype(f)}; path = "{f}"; sourceTree = "<group>"; }};\n')
o.append(f' {oid("appref")} = {{isa = PBXFileReference; explicitFileType = wrapper.application; path = {name}.app; sourceTree = BUILT_PRODUCTS_DIR; }};\n')
o.append(f' {oid("testref")} = {{isa = PBXFileReference; explicitFileType = wrapper.cfbundle; path = {name}Tests.xctest; sourceTree = BUILT_PRODUCTS_DIR; }};\n')
for f in app_sources:
    o.append(f' {oid("bf"+f)} = {{isa = PBXBuildFile; fileRef = {oid("ref"+f)}; }};\n')
for f in test_sources + test_resources:
    o.append(f' {oid("bf"+f)} = {{isa = PBXBuildFile; fileRef = {oid("ref"+f)}; }};\n')
kids = ",".join(oid("ref"+f) for f in files)
o.append(f' {oid("products")} = {{isa = PBXGroup; children = ({oid("appref")},{oid("testref")}); name = Products; sourceTree = "<group>"; }};\n')
o.append(f' {oid("main")} = {{isa = PBXGroup; children = ({kids},{oid("products")}); sourceTree = "<group>"; }};\n')
def phase(kind, key, fl):
    return f' {oid(key)} = {{isa = {kind}; buildActionMask = 2147483647; files = ({",".join(oid("bf"+f) for f in fl)}); runOnlyForDeploymentPostprocessing = 0; }};\n'
o.append(phase("PBXSourcesBuildPhase","appsrc",[f for f in app_sources if f.endswith((".swift",".c",".m"))]))
o.append(phase("PBXFrameworksBuildPhase","appfw",[f for f in app_sources if f.endswith((".a",".dylib"))]))
o.append(phase("PBXSourcesBuildPhase","testsrc",test_sources))
o.append(phase("PBXResourcesBuildPhase","testres",test_resources))
common = 'CODE_SIGN_IDENTITY = "-"; CODE_SIGN_STYLE = Manual; MACOSX_DEPLOYMENT_TARGET = 13.0; SDKROOT = macosx; SWIFT_VERSION = 5.0; ENABLE_HARDENED_RUNTIME = NO; GENERATE_INFOPLIST_FILE = YES; '
def configs(key, settings):
    o.append(f' {oid(key+"dbg")} = {{isa = XCBuildConfiguration; buildSettings = {{{common}{settings}}}; name = Debug; }};\n')
    o.append(f' {oid(key+"list")} = {{isa = XCConfigurationList; buildConfigurations = ({oid(key+"dbg")}); defaultConfigurationName = Debug; }};\n')
configs("proj", 'ONLY_ACTIVE_ARCH = YES; ')
configs("app", f'PRODUCT_BUNDLE_IDENTIFIER = {bundle}; PRODUCT_NAME = {name}; INFOPLIST_KEY_NSPrincipalClass = NSApplication; {extra_app_settings}')
configs("test", f'PRODUCT_BUNDLE_IDENTIFIER = {bundle}.tests; PRODUCT_NAME = {name}Tests; BUNDLE_LOADER = "$(TEST_HOST)"; TEST_HOST = "$(BUILT_PRODUCTS_DIR)/{name}.app/Contents/MacOS/{name}"; ')
o.append(f' {oid("apptarget")} = {{isa = PBXNativeTarget; buildConfigurationList = {oid("applist")}; buildPhases = ({oid("appsrc")},{oid("appfw")}); buildRules = (); dependencies = (); name = {name}; productName = {name}; productReference = {oid("appref")}; productType = "com.apple.product-type.application"; }};\n')
o.append(f' {oid("dep")} = {{isa = PBXTargetDependency; target = {oid("apptarget")}; targetProxy = {oid("proxy")}; }};\n')
o.append(f' {oid("proxy")} = {{isa = PBXContainerItemProxy; containerPortal = {oid("project")}; proxyType = 1; remoteGlobalIDString = {oid("apptarget")}; remoteInfo = {name}; }};\n')
o.append(f' {oid("testtarget")} = {{isa = PBXNativeTarget; buildConfigurationList = {oid("testlist")}; buildPhases = ({oid("testsrc")},{oid("testres")}); buildRules = (); dependencies = ({oid("dep")}); name = {name}Tests; productName = {name}Tests; productReference = {oid("testref")}; productType = "com.apple.product-type.bundle.unit-test"; }};\n')
o.append(f' {oid("project")} = {{isa = PBXProject; attributes = {{LastUpgradeCheck = 1600; TargetAttributes = {{{oid("testtarget")} = {{TestTargetID = {oid("apptarget")}; }}; }}; }}; buildConfigurationList = {oid("projlist")}; compatibilityVersion = "Xcode 14.0"; mainGroup = {oid("main")}; productRefGroup = {oid("products")}; projectDirPath = ""; projectRoot = ""; targets = ({oid("apptarget")},{oid("testtarget")}); }};\n')
o.append(f' }};\n rootObject = {oid("project")};\n}}\n')
os.makedirs(f"{root}/{name}.xcodeproj", exist_ok=True)
open(f"{root}/{name}.xcodeproj/project.pbxproj","w").write("".join(o))
