#include "../../code/client/cl_discord_proto.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int32_t le32( const char *p ) {
    return (int32_t)((unsigned char)p[0] | ((unsigned char)p[1] << 8) |
                     ((unsigned char)p[2] << 16) | ((unsigned char)p[3] << 24));
}

static void test_escape( void ) {
    char buf[64];
    int n = Discord_JsonEscape( buf, sizeof(buf), "a\"b\\c" );
    CHECK( strcmp( buf, "a\\\"b\\\\c" ) == 0 );
    CHECK( n == (int)strlen( buf ) );

    /* truncation never overflows and stays NUL-terminated */
    char small[4];
    Discord_JsonEscape( small, sizeof(small), "xxxxxxx" );
    CHECK( small[3] == '\0' );
}

static void test_handshake( void ) {
    char buf[512];
    int n = Discord_BuildHandshake( buf, sizeof(buf), "1517569490139746404" );
    CHECK( n > 8 );
    CHECK( le32( buf ) == 0 );                 /* opcode 0 */
    CHECK( le32( buf + 4 ) == n - 8 );         /* length = body size */
    CHECK( strstr( buf + 8, "\"v\":1" ) != NULL );
    CHECK( strstr( buf + 8, "1517569490139746404" ) != NULL );
}

static void test_set_activity( void ) {
    char buf[1024];
    discordActivity_t act;
    memset( &act, 0, sizeof(act) );
    strcpy( act.details, "Free For All" );
    strcpy( act.state, "q3dm17" );
    act.startTimestamp = 1700000000;

    int n = Discord_BuildSetActivity( buf, sizeof(buf), &act, 4242, 7 );
    CHECK( n > 8 );
    CHECK( le32( buf ) == 1 );                 /* opcode 1 */
    CHECK( le32( buf + 4 ) == n - 8 );
    CHECK( strstr( buf + 8, "\"cmd\":\"SET_ACTIVITY\"" ) != NULL );
    CHECK( strstr( buf + 8, "\"details\":\"Free For All\"" ) != NULL );
    CHECK( strstr( buf + 8, "\"state\":\"q3dm17\"" ) != NULL );
    CHECK( strstr( buf + 8, "\"start\":1700000000" ) != NULL );
    CHECK( strstr( buf + 8, "\"pid\":4242" ) != NULL );
}

static void test_clear_activity( void ) {
    char buf[512];
    int n = Discord_BuildClearActivity( buf, sizeof(buf), 4242, 8 );
    CHECK( n > 8 );
    CHECK( le32( buf ) == 1 );
    CHECK( strstr( buf + 8, "\"activity\":null" ) != NULL );
}

static void test_gametype_label( void ) {
    CHECK( strcmp( Discord_GametypeLabel( 0 ), "Free For All" ) == 0 );
    CHECK( strcmp( Discord_GametypeLabel( 4 ), "Capture the Flag" ) == 0 );
    CHECK( strcmp( Discord_GametypeLabel( 99 ), "Multiplayer" ) == 0 );
}

static void test_map_activity_menu( void ) {
    discordActivity_t a;
    Discord_MapActivity( &a, DISCORD_MENU, "", "", 0, 100, NULL );
    CHECK( strcmp( a.details, "In Menus" ) == 0 );
    CHECK( a.state[0] == '\0' );
    CHECK( a.startTimestamp == 100 );
}

static void test_map_activity_playing_reads_mapname( void ) {
    discordActivity_t a;
    const char *si = "\\mapname\\q3dm17\\sv_hostname\\secret\\g_gametype\\0";
    Discord_MapActivity( &a, DISCORD_PLAYING, si, "", 0, 200, NULL );
    CHECK( strcmp( a.details, "Free For All" ) == 0 );
    CHECK( strcmp( a.state, "q3dm17" ) == 0 );
    /* server name must never leak into either field */
    CHECK( strstr( a.details, "secret" ) == NULL );
    CHECK( strstr( a.state, "secret" ) == NULL );
}

static void test_timestamp_persists_across_same_activity( void ) {
    discordActivity_t prev, a;
    const char *si = "\\mapname\\q3dm17\\g_gametype\\0";
    Discord_MapActivity( &prev, DISCORD_PLAYING, si, "", 0, 200, NULL );
    Discord_MapActivity( &a, DISCORD_PLAYING, si, "", 0, 999, &prev );
    CHECK( a.startTimestamp == 200 );            /* same map -> keep timer */

    discordActivity_t b;
    const char *si2 = "\\mapname\\q3dm6\\g_gametype\\0";
    Discord_MapActivity( &b, DISCORD_PLAYING, si2, "", 0, 999, &prev );
    CHECK( b.startTimestamp == 999 );            /* map changed -> reset timer */
}

static void test_activity_equal( void ) {
    discordActivity_t a, b;
    memset( &a, 0, sizeof(a) ); memset( &b, 0, sizeof(b) );
    strcpy( a.details, "x" ); strcpy( b.details, "x" );
    a.startTimestamp = 1; b.startTimestamp = 2;  /* timestamp ignored */
    CHECK( Discord_ActivityEqual( &a, &b ) == 1 );
    strcpy( b.state, "y" );
    CHECK( Discord_ActivityEqual( &a, &b ) == 0 );
}

static void test_buf_contains_past_binary_header( void ) {
    /* A Discord response frame: 8-byte binary header [opcode=1][len=...]
       whose bytes 1..3 and 5..7 are NUL, followed by the JSON body. A plain
       strstr stops at the first NUL (byte 1) and never reaches the marker. */
    char frame[64];
    const char *body = "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\"}";
    int n;
    memset( frame, 0, 8 );
    frame[0] = 1;            /* opcode low byte; 1..3 stay NUL */
    frame[4] = (char)0x7f;   /* length low byte; 5..7 stay NUL */
    memcpy( frame + 8, body, strlen( body ) );
    n = 8 + (int)strlen( body );

    CHECK( Discord_BufContains( frame, n, "\"evt\":\"READY\"" ) == 1 );
    CHECK( Discord_BufContains( frame, n, "\"evt\":\"NOPE\"" ) == 0 );
    /* needle longer than buffer must not over-read */
    CHECK( Discord_BufContains( frame, 2, "\"evt\":\"READY\"" ) == 0 );
}

static void test_map_activity_watching( void ) {
    discordActivity_t a;
    const char *si = "\\mapname\\q3tourney2\\sv_hostname\\secret\\g_gametype\\3";
    /* gametype is irrelevant for watching phases; the label is fixed text */
    Discord_MapActivity( &a, DISCORD_WATCHING_DEMO, si, "", 3, 10, NULL );
    CHECK( strcmp( a.details, "Watching a demo" ) == 0 );
    CHECK( strcmp( a.state, "q3tourney2" ) == 0 );
    CHECK( strstr( a.details, "secret" ) == NULL );

    Discord_MapActivity( &a, DISCORD_WATCHING_TVD, si, "", 3, 10, NULL );
    CHECK( strcmp( a.details, "Watching a TrinityVision demo" ) == 0 );
    CHECK( strcmp( a.state, "q3tourney2" ) == 0 );

    Discord_MapActivity( &a, DISCORD_WATCHING_TV, si, "", 3, 10, NULL );
    CHECK( strcmp( a.details, "Watching TrinityVision" ) == 0 );
    CHECK( strcmp( a.state, "q3tourney2" ) == 0 );
}

static void test_map_activity_prefers_message( void ) {
    discordActivity_t a;
    const char *si = "\\mapname\\q3dm17\\sv_hostname\\secret\\g_gametype\\0";
    /* worldspawn message is preferred as the display name when present */
    Discord_MapActivity( &a, DISCORD_PLAYING, si, "The Longest Yard", 0, 1, NULL );
    CHECK( strcmp( a.state, "The Longest Yard" ) == 0 );
    CHECK( strstr( a.state, "secret" ) == NULL );
    /* empty message falls back to the bsp mapname */
    Discord_MapActivity( &a, DISCORD_PLAYING, si, "", 0, 1, NULL );
    CHECK( strcmp( a.state, "q3dm17" ) == 0 );
    /* Quake color codes in the message are stripped */
    Discord_MapActivity( &a, DISCORD_PLAYING, si, "^1The ^3Longest ^7Yard", 0, 1, NULL );
    CHECK( strcmp( a.state, "The Longest Yard" ) == 0 );
    /* a message that is only color codes falls back to the bsp mapname */
    Discord_MapActivity( &a, DISCORD_PLAYING, si, "^1^2^3", 0, 1, NULL );
    CHECK( strcmp( a.state, "q3dm17" ) == 0 );
}

static void test_set_activity_omits_empty_fields( void ) {
    /* Discord rejects an activity with an empty-string state/details
       ("not allowed to be empty"), so empty fields must be omitted entirely. */
    char buf[1024];
    discordActivity_t act;
    int n;

    memset( &act, 0, sizeof( act ) );
    strcpy( act.details, "In Menus" );   /* state left empty */
    act.startTimestamp = 123;
    n = Discord_BuildSetActivity( buf, sizeof( buf ), &act, 1, 1 );
    CHECK( n > 8 );
    CHECK( strstr( buf + 8, "\"details\":\"In Menus\"" ) != NULL );
    CHECK( strstr( buf + 8, "\"state\"" ) == NULL );      /* no empty state key */
    CHECK( strstr( buf + 8, "\"timestamps\"" ) != NULL );

    memset( &act, 0, sizeof( act ) );
    strcpy( act.state, "q3dm17" );        /* details left empty */
    n = Discord_BuildSetActivity( buf, sizeof( buf ), &act, 1, 2 );
    CHECK( strstr( buf + 8, "\"state\":\"q3dm17\"" ) != NULL );
    CHECK( strstr( buf + 8, "\"details\"" ) == NULL );    /* no empty details key */
}

int main( void ) {
    test_escape();
    test_handshake();
    test_set_activity();
    test_clear_activity();
    test_gametype_label();
    test_map_activity_menu();
    test_map_activity_playing_reads_mapname();
    test_timestamp_persists_across_same_activity();
    test_activity_equal();
    test_buf_contains_past_binary_header();
    test_map_activity_watching();
    test_map_activity_prefers_message();
    test_set_activity_omits_empty_fields();
    if ( failures ) { printf( "%d FAILURES\n", failures ); return 1; }
    printf( "all proto tests passed\n" );
    return 0;
}
