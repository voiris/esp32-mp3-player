#pragma once

/*
 * id3.h — Full-featured parser for ID3v1 and ID3v2 (2.2, 2.3, 2.4) metadata.
 *
 * Features:
 * - Support for all major ID3 versions.
 * - Automatic decoding of text encodings (Latin-1, UTF-16, UTF-8) into clean UTF-8.
 * - Handling of multi-value fields, album art, lyrics, and chapters.
 * - Robustness against corrupted data and unsynchronization.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Return Codes --- */
typedef enum {
    ID3_OK              =  0,
    ID3_ERR_FILE        = -1,   /* не удалось открыть/прочитать файл */
    ID3_ERR_NO_TAG      = -2,   /* ни одного тега не найдено */
    ID3_ERR_NOMEM       = -3,   /* ошибка выделения памяти */
    ID3_ERR_CORRUPT     = -4,   /* повреждённые данные тега */
} ID3Result;

/* --- Tag Format Versions --- */
typedef enum {
    ID3_VERSION_NONE  = 0,
    ID3_VERSION_1     = 1,   /* ID3v1.0 */
    ID3_VERSION_1_1   = 2,   /* ID3v1.1 (с track number) */
    ID3_VERSION_2_2   = 3,   /* ID3v2.2 */
    ID3_VERSION_2_3   = 4,   /* ID3v2.3 */
    ID3_VERSION_2_4   = 5,   /* ID3v2.4 */
} ID3Version;

/* --- Text Field ---
 * Always stored in UTF-8. If a frame contains multiple values (v2.4),
 * they are accessible via the `values` array.
 */
typedef struct {
    char    *value;     /* Primary value (null-terminated) or the first value in a list */
    uint32_t count;     /* Total number of values (for v2.4 multi-value frames) */
    char   **values;    /* Array of all values (NULL if only one value exists) */
} ID3TextField;

/* --- Image Data (APIC / PIC frames) --- */
typedef enum {
    ID3_PIC_OTHER            = 0x00,
    ID3_PIC_FILE_ICON        = 0x01,
    ID3_PIC_OTHER_ICON       = 0x02,
    ID3_PIC_FRONT_COVER      = 0x03,
    ID3_PIC_BACK_COVER       = 0x04,
    ID3_PIC_LEAFLET          = 0x05,
    ID3_PIC_MEDIA            = 0x06,
    ID3_PIC_LEAD_ARTIST      = 0x07,
    ID3_PIC_ARTIST           = 0x08,
    ID3_PIC_CONDUCTOR        = 0x09,
    ID3_PIC_BAND             = 0x0A,
    ID3_PIC_COMPOSER         = 0x0B,
    ID3_PIC_LYRICIST         = 0x0C,
    ID3_PIC_RECORDING_LOCATION = 0x0D,
    ID3_PIC_DURING_RECORDING = 0x0E,
    ID3_PIC_DURING_PERFORMANCE = 0x0F,
    ID3_PIC_VIDEO_CAPTURE    = 0x10,
    ID3_PIC_A_BRIGHT_COLOURED_FISH = 0x11,
    ID3_PIC_ILLUSTRATION     = 0x12,
    ID3_PIC_BAND_LOGOTYPE    = 0x13,
    ID3_PIC_PUBLISHER_LOGOTYPE = 0x14,
} ID3PictureType;

typedef struct {
    ID3PictureType  type;
    char            mime[32];        /* MIME type (e.g., "image/jpeg") */
    char            description[128]; /* Brief description of the image */
    uint8_t        *data;            /* Binary data (ownership transferred to the user) */
    uint32_t        size;            /* Data size in bytes */
} ID3Picture;

/* --- Comment (COMM frame) --- */
typedef struct {
    char lang[4];            /* ISO-639-2 language code (e.g., "eng") */
    char description[128];   /* Comment header or description */
    char *text;              /* Comment content (UTF-8) */
} ID3Comment;

/* --- Unsynchronized Lyrics (USLT frame) --- */
typedef struct {
    char lang[4];            /* Language code */
    char description[128];   /* Content description */
    char *text;              /* Full lyrics text (UTF-8) */
} ID3Lyrics;

/* --- User-defined Text Field (TXXX frame) --- */
typedef struct {
    char  key[128];          /* Field identifier (description) */
    char *value;             /* Field value (UTF-8) */
} ID3UserText;

/* --- URLs (WXXX and W??? frames) --- */
typedef struct {
    char description[128];   /* URL description (WXXX only) */
    char id[5];              /* Frame ID (e.g., "WOAR" for official artist webpage) */
    char *url;               /* URL string */
} ID3Url;

/* --- Chapter Data (CHAP frame) --- */
typedef struct {
    char     element_id[64]; /* Unique ID for the chapter within the file */
    uint32_t start_ms;       /* Chapter start time (ms) */
    uint32_t end_ms;         /* Chapter end time (ms) */
    uint32_t start_offset;   /* Byte offset (0xFFFFFFFF if unused) */
    uint32_t end_offset;     /* End byte offset */
} ID3Chapter;

/* --- Synchronized Lyrics (SYLT frame) --- */
typedef struct {
    uint32_t timestamp_ms;   /* Timestamp for this specific line */
    char    *text;           /* Line text */
} ID3SyltEntry;

typedef struct {
    char         lang[4];
    char         description[128];
    uint8_t      timestamp_format; /* Time units: 1=MPEG frames, 2=milliseconds */
    uint8_t      content_type;     /* Type: 1=lyrics, 2=chords, etc. */
    ID3SyltEntry *entries;         /* Array of timestamped entries */
    uint32_t      entry_count;
} ID3SyncLyrics;

/* --- General Encapsulated Object (GEOB frame) --- */
typedef struct {
    char     mime[64];       /* Object content type */
    char     filename[256];  /* Original filename */
    char     description[128];
    uint8_t *data;           /* Encapsulated data */
    uint32_t size;
} ID3Object;

/* --- Volume Levels and ReplayGain (RVA2 / RVAD frames) --- */
typedef struct {
    char  identification[64]; /* Usage identification (e.g., "master") */
    float track_gain_db;      /* Track gain adjustment in dB */
    float album_gain_db;      /* Album gain adjustment in dB */
    bool  has_album;          /* Flag indicating if album data is present */
} ID3ReplayGain;

/* --- Final Tag Result Structure --- */
typedef struct {
    /* Metadata about the tag itself */
    ID3Version  version;         /* Highest tag version found in the file */
    bool        has_v1;          /* Whether an ID3v1 tag was found */
    bool        has_v2;          /* Whether an ID3v2 tag was found */
    uint8_t     v2_major;        /* v2 Major version (2, 3, or 4) */
    uint8_t     v2_revision;     /* v2 Revision (minor version) */
    uint32_t    v2_tag_size;     /* Total size of the ID3v2 block (including headers) */

    /* Core Metadata Fields */
    ID3TextField title;          /* Track title */
    ID3TextField subtitle;       /* Subtitle (refinement) */
    ID3TextField content_group;  /* Content group (e.g., Symphony) */
    ID3TextField artist;         /* Lead artist */
    ID3TextField album_artist;   /* Album artist */
    ID3TextField conductor;      /* Conductor */
    ID3TextField remixer;        /* Interpreted by, remixed by */
    ID3TextField album;          /* Album title */
    ID3TextField track;          /* Track number string (e.g., "1/10") */
    uint32_t     track_number;   /* Numeric track number */
    uint32_t     track_total;    /* Total tracks in the album */
    ID3TextField disc;           /* Disc number string */
    uint32_t     disc_number;    /* Numeric disc number */
    uint32_t     disc_total;     /* Total discs in the set */
    ID3TextField year;           /* Release year (TYER / TDRC) */
    ID3TextField genre;          /* Genre */
    ID3TextField composer;       /* Composer */
    ID3TextField lyricist;       /* Lyricist / Text writer */
    ID3TextField publisher;      /* Publisher */
    ID3TextField copyright;      /* Copyright message */
    ID3TextField encoded_by;     /* Encoded by */
    ID3TextField encoder_settings; /* Encoder settings */
    ID3TextField isrc;           /* International Standard Recording Code (ISRC) */
    ID3TextField language;       /* Recording language */
    ID3TextField key;            /* Musical key */
    ID3TextField bpm;            /* Beats per minute */
    uint32_t     bpm_value;
    ID3TextField length_ms;      /* Length in ms (string) */
    uint32_t     length_ms_value;/* Numeric length */
    ID3TextField media_type;     /* Media type (e.g., "DIG") */
    ID3TextField original_title; /* Original title */
    ID3TextField original_artist;/* Original artist */
    ID3TextField original_year;  /* Original release year */

    /* Lists of complex structures (dynamically allocated) */
    ID3Picture    *pictures;
    uint32_t       picture_count;
    ID3Comment    *comments;
    uint32_t       comment_count;
    ID3Lyrics     *lyrics;
    uint32_t       lyrics_count;
    ID3UserText   *user_texts;
    uint32_t       user_text_count;
    ID3Url        *urls;
    uint32_t       url_count;
    ID3SyncLyrics *sync_lyrics;
    uint32_t       sync_lyrics_count;
    ID3Chapter    *chapters;
    uint32_t       chapter_count;
    ID3Object     *objects;
    uint32_t       object_count;

    ID3ReplayGain  replay_gain;
    bool           has_replay_gain;

    /* Unknown or unsupported frames */
    struct ID3RawFrame {
        char     id[5];          /* 4-character frame ID */
        uint8_t *data;           /* Raw frame data */
        uint32_t size;           /* Data size */
        uint16_t flags;          /* Frame flags */
    } *raw_frames;
    uint32_t raw_frame_count;

} ID3Tag;

/* --- API --- */

/* Parses ID3 tags from a file path. Handles file descriptors internally. */
ID3Result id3_parse_file(const char *path, ID3Tag *tag);

/* Parses from an open file stream. File position may change. */
ID3Result id3_parse_fp(FILE *fp, ID3Tag *tag);

/* Initializes the structure (zeros out all fields). */
void id3_tag_init(ID3Tag *tag);

/* Deep-frees all dynamically allocated memory within the structure. */
void id3_free(ID3Tag *tag);

/* Returns a string representation of the version (e.g., "ID3v2.3"). */
const char *id3_version_str(ID3Version v);

/* Returns a string description of the picture type (e.g., "Front cover"). */
const char *id3_picture_type_str(ID3PictureType t);

/* Helper to retrieve the primary text from a field. */
static const char *id3_text(const ID3TextField *f);

/* Dumps debug information about the tag to a stream. */
void id3_dump(const ID3Tag *tag, FILE *out);

#ifdef __cplusplus
}
#endif