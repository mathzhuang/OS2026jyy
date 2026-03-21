#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "testkit.h"
#include "labyrinth.h"

static const char *TEST_MAP = "test.map";

static void write_map_file(const char *contents) {
    FILE *f = fopen(TEST_MAP, "w");
    tk_assert(f != NULL, "Should be able to create %s", TEST_MAP);
    fputs(contents, f);
    fclose(f);
}

static char *read_map_file(void) {
    FILE *f = fopen(TEST_MAP, "r");
    tk_assert(f != NULL, "Should be able to open %s", TEST_MAP);

    tk_assert(fseek(f, 0, SEEK_END) == 0, "fseek() should succeed");
    long size = ftell(f);
    tk_assert(size >= 0, "ftell() should succeed");
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    tk_assert(buf != NULL, "malloc() should succeed");

    size_t nread = fread(buf, 1, (size_t)size, f);
    tk_assert(nread == (size_t)size, "Should read the whole map file");
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void cleanup_test_map(void) {
    remove(TEST_MAP);
}

static void setup_printable_map(void) {
    write_map_file("###\n#1.\n###\n");
}

static void setup_moveable_map(void) {
    write_map_file("1.\n..\n");
}

static void setup_spawn_map(void) {
    write_map_file("..\n##\n");
}

static void setup_wall_blocked_map(void) {
    write_map_file("1#\n..\n");
}

static void setup_open_map(void) {
    write_map_file("....\n....\n....\n");
}

static void setup_bad_shape_map(void) {
    write_map_file("...\n..\n");
}

static void setup_disconnected_map(void) {
    write_map_file(".#.\n###\n.#.\n");
}

SystemTest(test_version, ((const char *[]){ "./labyrinth", "--version" })) {
    tk_assert(result->exit_status == 0, "Must exit 0");
    tk_assert(
        strstr(result->output, "Labyrinth Game") != NULL,
        "Must have correct message"
    );
}

SystemTest(test_version_fail, ((const char *[]){ "./labyrinth", "--version", "??" })) {
    tk_assert(result->exit_status == 1, "Must exit 1");
}

SystemTest(invalid_args_1, ((const char *[]){ "./labyrinth", "--nonexist", "--another" })) {
    tk_assert(result->exit_status == 1, "Must exit 1");
}

SystemTest(invalid_args_2, ((const char *[]){ "./labyrinth", "hello os world" })) {
    tk_assert(result->exit_status == 1, "Must exit 1");
}

SystemTest(test_prints_map_verbatim,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "1" }),
    .init = setup_printable_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 0, "Must exit 0");
    tk_assert(strcmp(result->output, "###\n#1.\n###\n") == 0,
              "Map output must be printed verbatim, got:\n%s", result->output);
}

SystemTest(test_move_existing_player_updates_map,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "1", "--move", "right" }),
    .init = setup_moveable_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 0, "Valid move should exit 0");

    char *saved = read_map_file();
    tk_assert(strcmp(saved, ".1\n..\n") == 0,
              "Player move should be persisted, got:\n%s", saved);
    free(saved);
}

SystemTest(test_move_missing_player_uses_first_empty_space,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "2", "--move", "right" }),
    .init = setup_spawn_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 0,
              "A missing player should spawn at the first empty cell and move");

    char *saved = read_map_file();
    tk_assert(strcmp(saved, ".2\n##\n") == 0,
              "Spawned player should move from the first empty cell, got:\n%s",
              saved);
    free(saved);
}

SystemTest(test_move_into_wall_fails,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "1", "--move", "right" }),
    .init = setup_wall_blocked_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 1, "Blocked move should exit 1");

    char *saved = read_map_file();
    tk_assert(strcmp(saved, "1#\n..\n") == 0,
              "Failed moves must not rewrite the map, got:\n%s", saved);
    free(saved);
}

SystemTest(test_invalid_move_direction,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "1", "--move", "invalid" }),
    .init = setup_open_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 1,
              "Invalid move direction should return error");
}

SystemTest(test_invalid_player,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "X" })) {
    tk_assert(result->exit_status == 1, "Invalid player ID should return error");
}

SystemTest(test_missing_map_parameter,
    ((const char *[]){ "./labyrinth", "--player", "1" })) {
    tk_assert(result->exit_status == 1,
              "Missing --map should return an error");
}

SystemTest(test_missing_player_parameter,
    ((const char *[]){ "./labyrinth", "--map", "test.map" }),
    .init = setup_open_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 1,
              "Missing --player should return an error");
}

SystemTest(test_inconsistent_rows_rejected,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "1" }),
    .init = setup_bad_shape_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 1,
              "Malformed maps with uneven row lengths must be rejected");
}

SystemTest(test_disconnected_map_rejected,
    ((const char *[]){ "./labyrinth", "--map", "test.map", "--player", "1" }),
    .init = setup_disconnected_map,
    .fini = cleanup_test_map) {
    tk_assert(result->exit_status == 1,
              "Disconnected empty spaces must be rejected");
}


UnitTest(test_valid_player_id) {
    tk_assert(isValidPlayer('0') == true, "0 should be a valid player ID");
    tk_assert(isValidPlayer('9') == true, "9 should be a valid player ID");
    tk_assert(isValidPlayer('5') == true, "5 should be a valid player ID");
    tk_assert(isValidPlayer('a') == false, "Letters should not be valid player IDs");
    tk_assert(isValidPlayer('#') == false, "Special characters should not be valid player IDs");
}

UnitTest(test_empty_space) {
    Labyrinth lab = {
        .rows = 3,
        .cols = 3,
        .map = {
            "..#",
            "#..",
            "..."
        }
    };
    
    tk_assert(isEmptySpace(&lab, 0, 0) == true, "Should be an empty space");
    tk_assert(isEmptySpace(&lab, 0, 2) == false, "Wall position should not be empty");
    tk_assert(isEmptySpace(&lab, -1, 0) == false, "Outside boundary should not be empty");
    tk_assert(isEmptySpace(&lab, 3, 0) == false, "Outside boundary should not be empty");
}

// Test maze connectivity check
UnitTest(test_maze_connectivity) {
    // Test connected maze
    Labyrinth connected = {
        .rows = 3,
        .cols = 3,
        .map = {
            "...",
            ".#.",
            "..."
        }
    };
    tk_assert(isConnected(&connected) == true, "Connected maze should return true");

    // Test disconnected maze
    Labyrinth disconnected = {
        .rows = 3,
        .cols = 3,
        .map = {
            "..#",
            "###",
            "#.."
        }
    };
    tk_assert(isConnected(&disconnected) == false, "Disconnected maze should return false");
}


UnitTest(test_find_player) {
    Labyrinth lab = {
        .rows = 3,
        .cols = 3,
        .map = {
            "..1",
            "...",
            "..."
        }
    };
    
    Position pos = findPlayer(&lab, '1');
    tk_assert(pos.row == 0 && pos.col == 2, "Should find correct player position");
    
    pos = findPlayer(&lab, '2');
    tk_assert(pos.row == -1 && pos.col == -1, "Should return (-1,-1) for player not found");
}


UnitTest(test_find_first_empty) {
    Labyrinth lab = {
        .rows = 2,
        .cols = 2,
        .map = {
            "#.",
            "##"
        }
    };
    
    Position pos = findFirstEmptySpace(&lab);
    tk_assert(pos.row == 0 && pos.col == 1, "Should find first empty position");
}

UnitTest(test_find_first_empty_none) {
    Labyrinth lab = {
        .rows = 2,
        .cols = 2,
        .map = {
            "##",
            "##"
        }
    };

    Position pos = findFirstEmptySpace(&lab);
    tk_assert(pos.row == -1 && pos.col == -1,
              "Should report no empty space when the map is full");
}

UnitTest(test_move_player_rejects_occupied_cell) {
    Labyrinth lab = {
        .rows = 2,
        .cols = 2,
        .map = {
            "12",
            ".."
        }
    };

    tk_assert(movePlayer(&lab, '1', "right") == false,
              "Player should not move onto another player");
    tk_assert(strcmp(lab.map[0], "12") == 0 && strcmp(lab.map[1], "..") == 0,
              "Rejected moves must leave the map unchanged");
}

UnitTest(test_move_player_invalid_direction) {
    Labyrinth lab = {
        .rows = 2,
        .cols = 2,
        .map = {
            "1.",
            ".."
        }
    };

    tk_assert(movePlayer(&lab, '1', "diagonal") == false,
              "Unknown directions should be rejected");
    tk_assert(strcmp(lab.map[0], "1.") == 0 && strcmp(lab.map[1], "..") == 0,
              "Invalid directions must not change the map");
}
