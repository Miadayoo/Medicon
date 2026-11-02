// IDA MCP analysis of Minecraft.Windows.exe 1.26.60.5. Pattern data only.
// Evidence: docs/bedrock-1.26.60.05-auth-evidence.json. Live login is unverified.
constexpr SignatureSource source{
    "medicon:docs/bedrock-1.26.60.05-auth-evidence.json",
    "988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a",
    "1.26.60.05/Minecraft.Windows.exe", "project_research"};
constexpr SignatureDefinition definitions[]{
    {"ConnectionRequest_create", "55 41 56 56 57 53 48 81 EC C0 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 38 01 00 00 FE FF FF FF 4C 89 CB 4C 89 C7 48 89 95 20 01 00 00 48 89 CE 48 8D 4D 60 4C 89 C2 E8 ?? ?? ?? ?? 90 48 8D 8D", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"ConnectionAuthInfo_toJson", "55 56 57 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 89 D6 48 89 4D F0 31 D2 E8 ?? ?? ?? ?? 48 8D 56 10 48 8D 4D E0 E8 ?? ?? ?? ?? 90 48 8D 15 ?? ?? ?? ?? 48 8B 4D F0 E8 ?? ?? ?? ??", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"Social_UserImpl_create", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 02 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 80 01 00 00 48 C7 85 78 01 00 00 FE FF FF FF 4C 89 85 68 01 00 00 49 89 CE 48 89 95 40 01 00 00 48 8B", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"XboxLiveIdentity_getAuthToken", "55 41 56 56 57 53 48 81 EC 50 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 C0 00 00 00 48 C7 85 B8 00 00 00 FE FF FF FF 48 89 D6 0F B6 81 11 02 00 00 A8 01 75 ?? 4D 89 C1 48 8B 81 E0 01 00 00 4C", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"Xal_getTokenAndSignature_helper", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 40 01 00 00 48 C7 85 38 01 00 00 FE FF FF FF 4D 89 C6 48 89 CE 0F 10 05 ?? ?? ?? ?? 0F 29 45 20 0F 57", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"Xal_getTokenAndSignature_result_helper", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 A0 00 00 00 FE FF FF FF 48 89 CE 48 8B 4A 10 48 85 C9 0F 84 ?? ?? ?? ?? 4D 8B 28 8B 41 08 0F 1F 40 00", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"PlayerAuthentication_getGlobalToken", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 02 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 80 01 00 00 48 C7 85 78 01 00 00 FE FF FF FF 48 89 D6 48 89 CF 49 8B 08 49 C7 00 00 00 00 00 4C 8B 97", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"XboxLiveIdentity_updateProfile_continuation", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 10 01 00 00 FE FF FF FF 48 89 4D E0 48 8B 7A 08 41 0F 10 00 0F 29 45 C0 48 8B 1F 31 C0 86 83 11 02 00", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
    {"AuthServiceApi_startMultiplayerSession", "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 02 00 00 48 8D AC 24 80 00 00 00 0F 29 BD D0 01 00 00 0F 29 B5 C0 01 00 00 48 C7 85 B8 01 00 00 FE FF FF FF 4D 89 C5 48 89 55 B0 49 89 CE 0F 57", SignatureResolution::direct, 0, 0, "1.26.60.5", true},
};
