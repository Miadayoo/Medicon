// Hand-maintained from docs/alt-manager-1.26.52.03-signatures.json (the project's
// own IDA/PE analysis of Minecraft.Windows.exe 1.26.52.03). Pattern data only.
// runtime_validated is false for every entry: these are not confirmed against a
// running game. Keep this in sync with that JSON and docs/alt-manager-sdk-validation.md.
constexpr SignatureSource source{
    "medicon:docs/alt-manager-1.26.52.03-signatures.json",
    "47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942",
    "1.26.52.03/Minecraft.Windows.exe", "project_research"};
constexpr SignatureDefinition definitions[]{
    {"ConnectionRequest_create", "55 41 57 41 56 56 57 53 48 81 EC B8 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 30 01 00 00 FE FF FF FF 4C 89 CB 4C 89 C7", SignatureResolution::direct, 0, 14, "1.26.52.03", true},
    {"ConnectionAuthInfo_toJson", "55 56 57 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 89 D6 48 89 4D F0 31 D2 E8 ?? ?? ?? ?? 48 8D 56 10 48 8D 4D E0", SignatureResolution::direct, 0, 29, "1.26.52.03", true},
    {"Social_UserImpl_create", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 0A 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 40 0A 00 00 48 C7 85 38 0A 00 00 FE FF FF FF 4C 89 C7", SignatureResolution::direct, 0, 44, "1.26.52.03", true},
    {"XboxLiveIdentity_getAuthToken", "55 41 56 56 57 53 48 81 EC 40 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 B0 00 00 00 48 C7 85 A8 00 00 00 FE FF FF FF 48 89 D6 F6 81 F9 01 00 00 01", SignatureResolution::direct, 0, 59, "1.26.52.03", true},
    {"Xal_getTokenAndSignature_helper", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 40 01 00 00 48 C7 85 38 01 00 00 FE FF FF FF 4C 89 C7 48 89 CE", SignatureResolution::direct, 0, 74, "1.26.52.03", true},
    {"Xal_getTokenAndSignature_result_helper", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 A0 00 00 00 FE FF FF FF 48 89 CE 48 8B 4A 10", SignatureResolution::direct, 0, 89, "1.26.52.03", true},
    {"PlayerAuthentication_getGlobalToken", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 02 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 80 01 00 00 48 C7 85 78 01 00 00 FE FF FF FF 48 89 D6 48 89 CF", SignatureResolution::direct, 0, 104, "1.26.52.03", true},
    {"XboxLiveIdentity_updateProfile_continuation", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 10 01 00 00 FE FF FF FF 48 89 4D E0", SignatureResolution::direct, 0, 119, "1.26.52.03", true},
    {"AuthServiceApi_startMultiplayerSession", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 02 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 D0 01 00 00 48 C7 85 C8 01 00 00 FE FF FF FF 4C 89 85 70 01 00 00 48 89 55 A8", SignatureResolution::direct, 0, 134, "1.26.52.03", true},
};
