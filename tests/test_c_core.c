#define P2C_UNIT_TESTING 1
#include "../gtk-app/main.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
        printf("    [OK] %s\n", msg); \
    } else { \
        fprintf(stderr, "    [FAIL] %s (Line %d)\n", msg, __LINE__); \
        assert(cond); \
    } \
} while(0)

static void test_link_type_detection(void) {
    printf("  -> Testing detect_dlps_link_type...\n");
    TEST_ASSERT(detect_dlps_link_type("https://example.com/game.pkg") == DLPS_LINK_DIRECT_PKG, "Direct .pkg link");
    TEST_ASSERT(detect_dlps_link_type("https://example.com/UPDATE.PKG?token=abc") == DLPS_LINK_DIRECT_PKG, "Direct uppercase .PKG link with query");
    TEST_ASSERT(detect_dlps_link_type("https://www.mediafire.com/file/abc1234/game.pkg/file") == DLPS_LINK_MEDIAFIRE, "Mediafire file link");
    TEST_ASSERT(detect_dlps_link_type("https://1fichier.com/?abcdef12345") == DLPS_LINK_HOSTER, "1fichier hoster link");
    TEST_ASSERT(detect_dlps_link_type("https://downloadgameps3.net/archives/50580") == DLPS_LINK_PROTECTOR, "downloadgameps3.net protector link");
    TEST_ASSERT(detect_dlps_link_type("https://filecrypt.cc/Container/ABCDEF.html") == DLPS_LINK_PROTECTOR, "filecrypt.cc protector link");
    TEST_ASSERT(detect_dlps_link_type("https://akirabox.com/file/1234") == DLPS_LINK_HOSTER, "AkiraBox hoster link");
    TEST_ASSERT(detect_dlps_link_type("https://vikingfile.com/d/5678") == DLPS_LINK_HOSTER, "Vikingfile hoster link");
    TEST_ASSERT(detect_dlps_link_type("") == DLPS_LINK_UNKNOWN, "Empty string returns UNKNOWN");
    TEST_ASSERT(detect_dlps_link_type(NULL) == DLPS_LINK_UNKNOWN, "NULL returns UNKNOWN");
}

static void test_url_looks_direct_file(void) {
    printf("  -> Testing url_looks_direct_file...\n");
    TEST_ASSERT(url_looks_direct_file("https://cdn.example.com/games/CUSA12345.pkg"), ".pkg detected as direct");
    TEST_ASSERT(url_looks_direct_file("https://cdn.example.com/games/CUSA12345.PKG?token=xyz"), ".PKG with query detected as direct");
    TEST_ASSERT(url_looks_direct_file("https://hoster.com/dl/file.rar"), ".rar detected as direct archive");
    TEST_ASSERT(url_looks_direct_file("https://hoster.com/dl/game.part1.rar"), ".part1.rar detected as direct archive");
    TEST_ASSERT(url_looks_direct_file("https://hoster.com/dl/package.7z?dl=1"), ".7z detected as direct archive");
    TEST_ASSERT(url_looks_direct_file("https://hoster.com/dl/archive.zip"), ".zip detected as direct archive");
    TEST_ASSERT(url_looks_direct_file("https://cdn.example.com/game.iso"), ".iso detected as direct");
    TEST_ASSERT(url_looks_direct_file("https://cdn.example.com/game.xci"), ".xci detected as direct");
    TEST_ASSERT(url_looks_direct_file("https://cdn.example.com/game.nsp"), ".nsp detected as direct");

    /* Marvel Cosmic Invasion FileDitch direct stream URL (base64 encoded path) */
    const char *marvel_cdn = "https://alpha.staysharpsleeps8hours.xyz/MTc5MzIyMTcyMDp3Y3RDVWVLRGQ3TkdOd21kQTBmeXFROjY3MzAvYmFscGhhMjYvN2IzYzYxY2I1ZThiOGRmMTgxNzgvJTVCRExQU0dBTUUuQ09NJTVELU1fNTQzMDhfT1BPSVNTTzg5My5yYXI";
    TEST_ASSERT(url_looks_direct_file(marvel_cdn), "Marvel Cosmic Invasion FileDitch CDN URL detected as direct file");
    TEST_ASSERT(url_looks_direct_file("https://fishoilenjoyer.xyz/stream/123"), "fishoilenjoyer.xyz direct stream");
    TEST_ASSERT(url_looks_direct_file("https://download.akirabox.com/file/123"), "akirabox direct stream");
    TEST_ASSERT(url_looks_direct_file("https://vikingfile.com/d/123"), "vikingfile direct stream");
    TEST_ASSERT(url_looks_direct_file("https://pixeldrain.com/api/file/abc"), "pixeldrain direct stream");

    TEST_ASSERT(!url_looks_direct_file("https://downloadgameps3.net/archives/50580"), "HTML protector page is NOT direct file");
    TEST_ASSERT(!url_looks_direct_file("https://mediafire.com/file/abc123/file"), "Mediafire landing page is NOT direct file");
}

static void test_extract_url_ext(void) {
    printf("  -> Testing extract_url_ext...\n");
    char ext[32] = "";

    extract_url_ext("https://example.com/file.pkg", ext, sizeof(ext));
    TEST_ASSERT(strcmp(ext, "pkg") == 0, "Extracts 'pkg'");

    extract_url_ext("https://example.com/archive.part1.rar", ext, sizeof(ext));
    TEST_ASSERT(strcmp(ext, "rar") == 0, "Extracts 'rar' from .part1.rar");

    extract_url_ext("https://example.com/file.PKG?sig=123&exp=456", ext, sizeof(ext));
    TEST_ASSERT(strcasecmp(ext, "pkg") == 0, "Extracts 'pkg' despite query string");

    extract_url_ext("https://example.com/archive.7z", ext, sizeof(ext));
    TEST_ASSERT(strcmp(ext, "7z") == 0, "Extracts '7z'");

    /* Marvel Cosmic Invasion FileDitch direct stream URL (base64 encoded path) */
    const char *marvel_cdn = "https://alpha.staysharpsleeps8hours.xyz/MTc5MzIyMTcyMDp3Y3RDVWVLRGQ3TkdOd21kQTBmeXFROjY3MzAvYmFscGhhMjYvN2IzYzYxY2I1ZThiOGRmMTgxNzgvJTVCRExQU0dBTUUuQ09NJTVELU1fNTQzMDhfT1BPSVNTTzg5My5yYXI";
    extract_url_ext(marvel_cdn, ext, sizeof(ext));
    TEST_ASSERT(strcmp(ext, "rar") == 0, "Extracts 'rar' from Marvel Cosmic Invasion base64 CDN URL");

    extract_url_ext("https://example.com/noextension", ext, sizeof(ext));
    TEST_ASSERT(ext[0] == '\0', "Empty extension for URL without extension");
}

static void test_is_known_ext(void) {
    printf("  -> Testing is_known_ext...\n");
    TEST_ASSERT(is_known_ext(".pkg"), ".pkg is known");
    TEST_ASSERT(is_known_ext(".PKG"), ".PKG is known");
    TEST_ASSERT(is_known_ext(".vpk"), ".vpk is known");
    TEST_ASSERT(is_known_ext(".nro"), ".nro is known");
    TEST_ASSERT(is_known_ext(".rar"), ".rar is known");
    TEST_ASSERT(is_known_ext(".7z"), ".7z is known");
    TEST_ASSERT(is_known_ext(".zip"), ".zip is known");
    TEST_ASSERT(is_known_ext(".iso"), ".iso is known");
    TEST_ASSERT(!is_known_ext(".exe"), ".exe is unknown");
    TEST_ASSERT(!is_known_ext(".html"), ".html is unknown");
    TEST_ASSERT(!is_known_ext(NULL), "NULL is unknown");
}

static void test_url_is_protected(void) {
    printf("  -> Testing url_is_protected...\n");
    TEST_ASSERT(url_is_protected("https://downloadgameps3.net/archives/12345"), "downloadgameps3.net is protected");
    TEST_ASSERT(url_is_protected("https://filecrypt.cc/Container/ABCDEF.html"), "filecrypt.cc is protected");
    TEST_ASSERT(url_is_protected("https://dlpsgame.com/archives/99999"), "dlpsgame.com is protected");
    TEST_ASSERT(!url_is_protected("https://github.com/releases/game.pkg"), "github direct is not protected");
    TEST_ASSERT(!url_is_protected("https://buildbot.libretro.com/stable/"), "buildbot is not protected");
}

static void test_dlps_extract_slug(void) {
    printf("  -> Testing dlps_extract_slug...\n");
    char slug[256] = "";
    char dom[256] = "";

    gboolean ok = dlps_extract_slug("https://dlpsgame.com/elden-ring-ps4/", slug, sizeof(slug), dom, sizeof(dom));
    TEST_ASSERT(ok && strcmp(slug, "elden-ring-ps4") == 0, "Extract slug elden-ring-ps4");

    ok = dlps_extract_slug("https://downloadgameps3.net/gran-turismo-7-ps4-free/", slug, sizeof(slug), dom, sizeof(dom));
    TEST_ASSERT(ok && strcmp(slug, "gran-turismo-7-ps4-free") == 0, "Extract slug gran-turismo-7");

    ok = dlps_extract_slug("cusa05999", slug, sizeof(slug), dom, sizeof(dom));
    TEST_ASSERT(!ok, "Plain search term is not a slug");
}

static void test_catalog_json_parser(void) {
    printf("  -> Testing parse_catalog_json...\n");
    const char *sample_json =
        "{"
        "  \"name\": \"Test Feed\","
        "  \"items\": ["
        "    {"
        "      \"id\": \"test-app\","
        "      \"name\": \"Test App\","
        "      \"version\": \"1.0.0\","
        "      \"category\": \"Utility\","
        "      \"region\": \"ALL\","
        "      \"url\": \"https://example.com/test.pkg\","
        "      \"pkg_name\": \"test.pkg\","
        "      \"size\": 1048576"
        "    }"
        "  ]"
        "}";

    CatalogItem items[10];
    int parsed = parse_catalog_json(sample_json, strlen(sample_json), "https://test.feed", items, 10);
    TEST_ASSERT(parsed == 1, "Parsed exactly 1 catalog item");
    TEST_ASSERT(strcmp(items[0].id, "test-app") == 0, "Item ID matches");
    TEST_ASSERT(strcmp(items[0].name, "Test App") == 0, "Item Name matches");
    TEST_ASSERT(strcmp(items[0].pkg_name, "test.pkg") == 0, "Item pkg_name matches");
    TEST_ASSERT(items[0].size == 1048576, "Item size matches");

    int bad = parse_catalog_json("{ invalid json... ", 18, "bad", items, 10);
    TEST_ASSERT(bad == 0, "Gracefully handles malformed JSON without crashing");
}

static void test_marvel_cosmic_invasion(void) {
    printf("  -> Testing Marvel Cosmic Invasion DLPS payload resolution...\n");
    const char *marvel_payload_html =
        "<p>Thank @Opoisso893/Golemnight</p>"
        "<p>CUSA54308 – EUR</p>"
        "<p>Game + Update 1.07 (Fix 5.05/6.72/7.xx/9.00/11.00/12.00) + DLC : "
        "<a href=\"https://downloadgameps3.net/archives/47392\" target=\"_blank\">Akia</a> – "
        "<a href=\"https://downloadgameps3.net/archives/47392\" target=\"_blank\">Rootz</a> – "
        "<a href=\"https://downloadgameps3.net/archives/47392\" target=\"_blank\">Data</a> – "
        "<a href=\"https://downloadgameps3.net/archives/47392\" target=\"_blank\">Viki</a> – "
        "<a href=\"https://downloadgameps3.net/archives/47392\" target=\"_blank\">Dith</a> – "
        "<a href=\"https://downloadgameps3.net/archives/47380\" target=\"_blank\">1File</a></p>";

    DlpsGame g = { 0 };
    g_strlcpy(g.title, "Marvel Cosmic Invasion", sizeof(g.title));
    extract_dlps_region(NULL, marvel_payload_html, &g);
    extract_dlps_links(marvel_payload_html, &g);

    TEST_ASSERT(g.n_links == 2, "Marvel Cosmic Invasion deduplicates identical URLs into 2 unique mirrors");
    TEST_ASSERT(strcmp(g.region, "EUR") == 0, "Marvel Cosmic Invasion region is EUR");
    TEST_ASSERT(strcmp(g.links[0].url, "https://downloadgameps3.net/archives/47392") == 0, "First mirror URL is downloadgameps3.net/archives/47392");
    TEST_ASSERT(strcmp(g.links[1].url, "https://downloadgameps3.net/archives/47380") == 0, "1File mirror URL is downloadgameps3.net/archives/47380");
}

static void test_archive_safety(void) {
    printf("  -> Testing archive safety and extracted file selection...\n");
    TEST_ASSERT(is_archive_filename("test.rar"), "test.rar is archive");
    TEST_ASSERT(is_archive_filename("[DLPSGAME.COM]-M_54308_OPOISSO893.rar"), "DLPS RAR is archive");
    TEST_ASSERT(is_archive_filename("archive.part1.rar"), "part1.rar is archive");
    TEST_ASSERT(is_archive_filename("file.zip"), "file.zip is archive");
    TEST_ASSERT(is_archive_filename("file.7z"), "file.7z is archive");
    TEST_ASSERT(!is_archive_filename("game.pkg"), "game.pkg is not archive");
    TEST_ASSERT(!is_archive_filename("game.iso"), "game.iso is not archive");

    TEST_ASSERT(is_console_game_ext("pkg"), "pkg is console game ext");
    TEST_ASSERT(is_console_game_ext("iso"), "iso is console game ext");
    TEST_ASSERT(!is_console_game_ext("rar"), "rar is not console game ext");

    /* Test that find_largest_file_recursive completely ignores archives even if larger! */
    char tmp_template[] = "/tmp/p2c_test_arch_XXXXXX";
    char *tmp_dir = mkdtemp(tmp_template);
    TEST_ASSERT(tmp_dir != NULL, "Created temporary test directory");

    if (tmp_dir) {
        char rar_file[512], pkg_file[512], txt_file[512];
        snprintf(rar_file, sizeof(rar_file), "%s/[DLPSGAME.COM]-M_54308.rar", tmp_dir);
        snprintf(pkg_file, sizeof(pkg_file), "%s/game.pkg", tmp_dir);
        snprintf(txt_file, sizeof(txt_file), "%s/instructions.txt", tmp_dir);

        /* Write 5000 bytes into .rar file (larger than .pkg!) */
        FILE *f_rar = fopen(rar_file, "wb");
        if (f_rar) {
            char zeroes[5000] = {0};
            fwrite(zeroes, 1, sizeof(zeroes), f_rar);
            fclose(f_rar);
        }

        /* Write 3000 bytes into .pkg file */
        FILE *f_pkg = fopen(pkg_file, "wb");
        if (f_pkg) {
            char zeroes[3000] = {0};
            fwrite(zeroes, 1, sizeof(zeroes), f_pkg);
            fclose(f_pkg);
        }

        /* Write 100 bytes into .txt file */
        FILE *f_txt = fopen(txt_file, "wb");
        if (f_txt) {
            char zeroes[100] = {0};
            fwrite(zeroes, 1, sizeof(zeroes), f_txt);
            fclose(f_txt);
        }

        char best_path[1024] = "";
        off_t best_size = -1;
        find_largest_file_recursive(tmp_dir, best_path, &best_size);

        TEST_ASSERT(strstr(best_path, "game.pkg") != NULL, "Extracted search selected game.pkg instead of larger .rar archive");
        TEST_ASSERT(best_size == 3000, "Best size matches game.pkg size (3000 bytes)");
        TEST_ASSERT(strstr(best_path, ".rar") == NULL, "Selected file is NEVER the .rar archive");

        cleanup_unneeded_extracted_files(tmp_dir, best_path);
        TEST_ASSERT(access(pkg_file, F_OK) == 0, "Target game.pkg was kept");
        TEST_ASSERT(access(rar_file, F_OK) != 0, "Useless .rar parts were deleted");
        TEST_ASSERT(access(txt_file, F_OK) != 0, "Useless .txt files were deleted");

        unlink(pkg_file);
        rmdir(tmp_dir);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_ALL);

    printf("--- [TEST 2/5] Running C Core Unit Tests ---\n");
    test_link_type_detection();
    test_url_looks_direct_file();
    test_extract_url_ext();
    test_is_known_ext();
    test_url_is_protected();
    test_dlps_extract_slug();
    test_catalog_json_parser();
    test_marvel_cosmic_invasion();
    test_archive_safety();

    printf("\n  Results: %d of %d C unit tests PASSED.\n", g_tests_passed, g_tests_run);
    curl_global_cleanup();

    if (g_tests_passed == g_tests_run) {
        printf("  [PASS] All C core unit tests completed successfully!\n\n");
        return 0;
    } else {
        printf("  [FAIL] Some C unit tests failed!\n\n");
        return 1;
    }
}
