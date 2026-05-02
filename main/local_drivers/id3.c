/*
 * id3.c — Complete ID3v1 / ID3v2.2 / ID3v2.3 / ID3v2.4 parser.
 *
 * Supports:
 *   - ID3v1.0 and v1.1 (128 bytes at end of file)
 *   - ID3v2.2, v2.3, v2.4 (at beginning or end of file)
 *   - All text frames (T???), APIC/PIC, COMM, USLT, SYLT,
 *     TXXX, W???/WXXX, GEOB, CHAP, RVA2
 *   - Unsynchronisation (v2.3 global, v2.4 per-frame)
 *   - Extended header (skipped)
 *   - Multi-value frames (v2.4, \0 separator)
 *   - Conversion Latin-1, UTF-16 LE/BE, UTF-8 → UTF-8
 *   - ID3v2.4 footer (tag search at end of file)
 */

#define _POSIX_C_SOURCE 200809L
#include "id3.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <inttypes.h>

/* Internal helper macros */
#define ID3_MIN(a,b) ((a)<(b)?(a):(b))

/* safe realloc: returns NULL on error, ptr remains unchanged */
static void *safe_realloc(void *ptr, const size_t size) {
    void *p = realloc(ptr, size);
    return p; /* caller проверяет */
}

static void textfield_free(ID3TextField *f) {
    if (!f) return;
    free(f->value);
    if (f->values) {
        for (uint32_t i = 0; i < f->count; i++) free(f->values[i]);
        free(f->values);
    }
    memset(f, 0, sizeof(*f));
}

static void textfield_set(ID3TextField *f, char *s) {
    /* s is passed as a malloc'd string */
    textfield_free(f);
    f->value = s;
    f->count = 1;
}

static void textfield_add(ID3TextField *f, char *s) {
    if (!f->value) { textfield_set(f, s); return; }
    f->count++;
    char **nv = safe_realloc(f->values, f->count * sizeof(char *));
    if (!nv) { free(s); return; }
    f->values = nv;
    /* copy value[0] on first addition */
    if (f->count == 2) {
        f->values[0] = f->value; /* transfer ownership */
        f->value = f->values[0]; /* value всегда == values[0] */
    }
    f->values[f->count - 1] = s;
}

/* Latin-1 byte → UTF-8 in buf (max 2 bytes), returns length */
static int latin1_to_utf8_char(uint8_t c, char *buf) {
    if (c < 0x80) { buf[0] = (char)c; return 1; }
    buf[0] = (char)(0xC0 | (c >> 6));
    buf[1] = (char)(0x80 | (c & 0x3F));
    return 2;
}

/* Converts Latin-1 buffer to UTF-8 malloc'd string */
static char *latin1_to_utf8(const uint8_t *src, const uint32_t len) {
    /* worst case: each byte → 2 UTF-8 bytes */
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    uint32_t j = 0;
    for (uint32_t i = 0; i < len; i++)
        j += latin1_to_utf8_char(src[i], out + j);
    out[j] = '\0';
    return out;
}

/* UTF-16 code point → UTF-8, returns number of written bytes */
static int utf16cp_to_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp; return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6)  & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/*
 * Converts UTF-16 (LE or BE) to UTF-8.
 * little_endian: 1=LE, 0=BE.
 * Handles surrogate pairs.
 */
static char *utf16_to_utf8(const uint8_t *src, uint32_t len, int little_endian) {
    if (len < 2) return strdup("");
    /* Skip BOM if present */
    if (len >= 2 &&
        ((src[0] == 0xFF && src[1] == 0xFE) ||
         (src[0] == 0xFE && src[1] == 0xFF))) {
        little_endian = (src[0] == 0xFF);
        src += 2; len -= 2;
    }
    /* Allocate with reserve: every 2 bytes → max 4 UTF-8 bytes */
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    uint32_t j = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        uint16_t w = little_endian
            ? (uint16_t)((src[i+1] << 8) | src[i])
            : (uint16_t)((src[i]   << 8) | src[i+1]);
        uint32_t cp = w;
        /* Surrogate pair */
        if (w >= 0xD800 && w <= 0xDBFF && i + 3 < len) {
            uint16_t w2 = little_endian
                ? (uint16_t)((src[i+3] << 8) | src[i+2])
                : (uint16_t)((src[i+2] << 8) | src[i+3]);
            if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                cp = 0x10000 + ((uint32_t)(w - 0xD800) << 10) + (w2 - 0xDC00);
                i += 2;
            }
        }
        if (cp == 0) break; /* null terminator */
        j += utf16cp_to_utf8(cp, out + j);
    }
    out[j] = '\0';
    return out;
}

/*
 * Decodes text buffer with an encoding byte at the beginning.
 * enc_byte = data[0].
 * Returns malloc'd UTF-8 string.
 */
static char *decode_text(const uint8_t *data, uint32_t size) {
    if (size == 0) return strdup("");
    uint8_t enc = data[0];
    const uint8_t *text = data + 1;
    uint32_t tlen = size - 1;

    /* Trim trailing nulls */
    while (tlen > 0) {
        if (enc == 0x01 || enc == 0x02) {
            if (tlen >= 2 && text[tlen-1] == 0 && text[tlen-2] == 0) tlen -= 2;
            else break;
        } else {
            if (text[tlen-1] == 0) tlen--;
            else break;
        }
    }

    switch (enc) {
        case 0x00: return latin1_to_utf8(text, tlen);
        case 0x01: return utf16_to_utf8(text, tlen, 1); /* LE с BOM */
        case 0x02: return utf16_to_utf8(text, tlen, 0); /* BE */
        case 0x03: { /* UTF-8 */
            char *s = malloc(tlen + 1);
            if (!s) return NULL;
            memcpy(s, text, tlen);
            s[tlen] = '\0';
            return s;
        }
        default: return latin1_to_utf8(text, tlen);
    }
}

/*
 * Reads null-terminated string in specified encoding from buffer.
 * Returns bytes consumed (including terminator).
 * out: malloc'd UTF-8.
 */
static uint32_t read_encoded_str(const uint8_t *buf, uint32_t max,
                                  uint8_t enc, char **out) {
    uint32_t len = 0;
    if (enc == 0x01 || enc == 0x02) {
        /* two-byte terminator */
        while (len + 1 < max && !(buf[len] == 0 && buf[len+1] == 0)) len += 2;
        if (len + 1 < max) {
            uint8_t *tmp2 = malloc(len + 1);
            if (tmp2) {
                tmp2[0] = enc;
                memcpy(tmp2 + 1, buf, len);
                *out = decode_text(tmp2, len + 1);
                free(tmp2);
            } else *out = strdup("");
            return len + 2; /* +terminator */
        }
        *out = strdup("");
        return len;
    } else {
        while (len < max && buf[len] != 0) len++;
        uint8_t *tmp = malloc(len + 1);
        if (tmp) {
            tmp[0] = enc;
            memcpy(tmp + 1, buf, len);
            *out = decode_text(tmp, len + 1);
            free(tmp);
        } else *out = strdup("");
        return len + 1;
    }
}

static uint32_t synchsafe_decode(const uint8_t b[4]) {
    return ((uint32_t)(b[0] & 0x7F) << 21)
         | ((uint32_t)(b[1] & 0x7F) << 14)
         | ((uint32_t)(b[2] & 0x7F) <<  7)
         |  (uint32_t)(b[3] & 0x7F);
}

static uint32_t be32(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
}

static uint32_t be24(const uint8_t b[3]) {
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}

static uint32_t be16(const uint8_t b[2]) {
    return ((uint32_t)b[0] << 8) | b[1];
}

/*
 * Unsynchronisation: decodes block on-the-fly.
 * Returns malloc'd buffer and size via out_size.
 */
static uint8_t *unsync_decode(const uint8_t *src, uint32_t src_len,
                               uint32_t *out_size) {
    /* Подсчитываем результирующий размер */
    uint32_t dst_len = 0;
    for (uint32_t i = 0; i < src_len; i++) {
        dst_len++;
        if (src[i] == 0xFF && i + 1 < src_len && src[i+1] == 0x00)
            i++; /* пропускаем вставленный 0x00 */
    }
    uint8_t *dst = malloc(dst_len);
    if (!dst) { *out_size = 0; return NULL; }
    uint32_t j = 0;
    for (uint32_t i = 0; i < src_len; i++) {
        dst[j++] = src[i];
        if (src[i] == 0xFF && i + 1 < src_len && src[i+1] == 0x00)
            i++;
    }
    *out_size = dst_len;
    return dst;
}

/* Parse string "4/12" → track_number, track_total */
static void parse_slash_number(const char *s,
                                uint32_t *num, uint32_t *total) {
    if (!s || !*s) return;
    *num = (uint32_t)atoi(s);
    const char *slash = strchr(s, '/');
    if (slash) *total = (uint32_t)atoi(slash + 1);
}


static const char *id3v1_genres[] = {
    "Blues","Classic Rock","Country","Dance","Disco","Funk","Grunge",
    "Hip-Hop","Jazz","Metal","New Age","Oldies","Other","Pop","Rhythm and Blues",
    "Rap","Reggae","Rock","Techno","Industrial","Alternative","Ska",
    "Death Metal","Pranks","Soundtrack","Euro-Techno","Ambient","Trip-Hop",
    "Vocal","Jazz & Funk","Fusion","Trance","Classical","Instrumental","Acid",
    "House","Game","Sound Clip","Gospel","Noise","Alternative Rock","Bass",
    "Soul","Punk","Space","Meditative","Instrumental Pop","Instrumental Rock",
    "Ethnic","Gothic","Darkwave","Techno-Industrial","Electronic","Pop-Folk",
    "Eurodance","Dream","Southern Rock","Comedy","Cult","Gangsta","Top 40",
    "Christian Rap","Pop/Funk","Jungle","Native US","Cabaret","New Wave",
    "Psychedelic","Rave","Showtunes","Trailer","Lo-Fi","Tribal","Acid Punk",
    "Acid Jazz","Polka","Retro","Musical","Rock & Roll","Hard Rock"
};
#define ID3V1_GENRE_COUNT (sizeof(id3v1_genres)/sizeof(id3v1_genres[0]))

/* Normalizes TCON genre to readable string */
static char *normalize_genre(const char *raw) {
    if (!raw || !*raw) return strdup("");
    char *result = strdup(raw);
    if (!result) return NULL;

    if (raw[0] == '(') {
        const char *p = raw + 1;
        if (p[0] == 'R' && p[1] == 'X' && p[2] == ')') {
            free(result); return strdup("Remix");
        }
        if (p[0] == 'C' && p[1] == 'R' && p[2] == ')') {
            free(result); return strdup("Cover");
        }
        char *end;
        long idx = strtol(p, &end, 10);
        if (*end == ')' && idx >= 0 && (size_t)idx < ID3V1_GENRE_COUNT) {
            const char *extra = end + 1;
            free(result);
            if (*extra && *extra != '(') {
                size_t need = strlen(id3v1_genres[idx]) + strlen(extra) + 3;
                result = malloc(need);
                if (result) snprintf(result, need, "%s (%s)",
                                     id3v1_genres[idx], extra);
            } else {
                result = strdup(id3v1_genres[idx]);
            }
        }
    }
    return result;
}

/* Parse RVA2 (Relative Volume Adjustment v2) */
static void parse_rva2(const uint8_t *data, uint32_t size, ID3Tag *tag) {
    if (size < 4) return;
    /* identification string (null-terminated) */
    uint32_t id_len = 0;
    while (id_len < size && data[id_len] != 0) id_len++;
    if (id_len >= size) return;

    const uint8_t *p = data + id_len + 1;
    uint32_t rem = size - id_len - 1;

    while (rem >= 4) {
        uint8_t channel = p[0];
        int16_t vol_adj = (int16_t)((p[1] << 8) | p[2]); /* in 1/512 dB */
        uint8_t peak_bits = p[3];
        uint32_t peak_bytes = (peak_bits + 7) / 8;
        p += 4 + peak_bytes;
        rem -= (4 + peak_bytes < rem) ? 4 + peak_bytes : rem;

        float db = vol_adj / 512.0f;
        if (!tag->has_replay_gain) {
            memset(&tag->replay_gain, 0, sizeof(tag->replay_gain));
            uint32_t copy = id_len < 63 ? id_len : 63;
            memcpy(tag->replay_gain.identification, data, copy);
            tag->replay_gain.identification[copy] = '\0';
            tag->has_replay_gain = true;
        }
        if (channel == 1) tag->replay_gain.track_gain_db = db;       /* master */
        if (channel == 2) { tag->replay_gain.album_gain_db = db;
                            tag->replay_gain.has_album = true; }
    }
}

/* Search for ReplayGain in TXXX keys */
static void check_replaygain_txxx(ID3Tag *tag, const char *key, const char *val) {
    if (!key || !val) return;
    char lkey[64]; uint32_t i;
    for (i = 0; i < 63 && key[i]; i++) lkey[i] = (char)tolower((unsigned char)key[i]);
    lkey[i] = '\0';

    if (!tag->has_replay_gain) {
        memset(&tag->replay_gain, 0, sizeof(tag->replay_gain));
        tag->has_replay_gain = true;
    }
    if (strstr(lkey, "replaygain_track_gain"))
        tag->replay_gain.track_gain_db = strtof(val, NULL);
    else if (strstr(lkey, "replaygain_album_gain")) {
        tag->replay_gain.album_gain_db = strtof(val, NULL);
        tag->replay_gain.has_album = true;
    }
}

/*
 * Process single ID3v2 frame.
 * data: frame body, size: length, id: ID string, major: version (2,3,4)
 */
static void process_frame(ID3Tag *tag, const char *id,
                           const uint8_t *data, uint32_t size,
                           uint8_t major) {
    if (size == 0) return;

/* Maps v2.2 (3-char) and v2.3/v2.4 (4-char) IDs */
#define IS(id4, id3_22) \
    (major == 2 ? strcmp(id, id3_22) == 0 : strcmp(id, id4) == 0)

#define SET_TEXT(field) do { \
    char *_s = decode_text(data, size); \
    if (_s) { \
        if (major == 4) { \
            /* v2.4: split by \0 */ \
            uint8_t enc = data[0]; \
            const uint8_t *tp = data + 1; uint32_t tsz = size - 1; \
            uint32_t ti = 0; bool first = true; \
            while (ti < tsz) { \
                uint32_t start = ti; \
                if (enc == 0x01 || enc == 0x02) { \
                    while (ti+1 < tsz && !(tp[ti]==0 && tp[ti+1]==0)) ti += 2; \
                    uint8_t *tmp3 = malloc(ti-start+1); \
                    if (tmp3) { tmp3[0]=enc; memcpy(tmp3+1,tp+start,ti-start); \
                        char *sv = decode_text(tmp3, ti-start+1); free(tmp3); \
                        if (sv) { if(first){textfield_set(&tag->field,sv);first=false;} \
                                  else textfield_add(&tag->field,sv); } } \
                    ti += 2; \
                } else { \
                    while (ti < tsz && tp[ti] != 0) ti++; \
                    uint8_t *tmp3 = malloc(ti-start+1); \
                    if (tmp3) { tmp3[0]=enc; memcpy(tmp3+1,tp+start,ti-start); \
                        char *sv = decode_text(tmp3, ti-start+1); free(tmp3); \
                        if (sv) { if(first){textfield_set(&tag->field,sv);first=false;} \
                                  else textfield_add(&tag->field,sv); } } \
                    ti++; \
                } \
            } \
            free(_s); \
        } else { textfield_set(&tag->field, _s); } \
    } \
} while(0)
    if (IS("TIT2","TT2")) { SET_TEXT(title); }
    else if (IS("TIT3","TT3")) { SET_TEXT(subtitle); }
    else if (IS("TIT1","TT1")) { SET_TEXT(content_group); }
    else if (IS("TPE1","TP1")) { SET_TEXT(artist); }
    else if (IS("TPE2","TP2")) { SET_TEXT(album_artist); }
    else if (IS("TPE3","TP3")) { SET_TEXT(conductor); }
    else if (IS("TPE4","TP4")) { SET_TEXT(remixer); }
    else if (IS("TALB","TAL")) { SET_TEXT(album); }
    else if (IS("TRCK","TRK")) {
        SET_TEXT(track);
        parse_slash_number(tag->track.value, &tag->track_number, &tag->track_total);
    }
    else if (IS("TPOS","TPA")) {
        SET_TEXT(disc);
        parse_slash_number(tag->disc.value, &tag->disc_number, &tag->disc_total);
    }
    else if (IS("TYER","TYE") || IS("TDRC","TDR") || IS("TDAT","TDA") ||
             IS("TDRL","TDL")) { SET_TEXT(year); }
    else if (IS("TCON","TCO")) {
        char *raw = decode_text(data, size);
        if (raw) {
            char *norm = normalize_genre(raw);
            free(raw);
            if (norm) textfield_set(&tag->genre, norm);
        }
    }
    else if (IS("TCOM","TCM")) { SET_TEXT(composer); }
    else if (IS("TEXT","TXT")) { SET_TEXT(lyricist); }
    else if (IS("TPUB","TPB")) { SET_TEXT(publisher); }
    else if (IS("TCOP","TCR")) { SET_TEXT(copyright); }
    else if (IS("TENC","TEN")) { SET_TEXT(encoded_by); }
    else if (IS("TSSE","TSS")) { SET_TEXT(encoder_settings); }
    else if (IS("TSRC","TRC")) { SET_TEXT(isrc); }
    else if (IS("TLAN","TLA")) { SET_TEXT(language); }
    else if (IS("TKEY","TKE")) { SET_TEXT(key); }
    else if (IS("TBPM","TBP")) {
        SET_TEXT(bpm);
        if (tag->bpm.value) tag->bpm_value = (uint32_t)atoi(tag->bpm.value);
    }
    else if (IS("TLEN","TLE")) {
        SET_TEXT(length_ms);
        if (tag->length_ms.value)
            tag->length_ms_value = (uint32_t)atoi(tag->length_ms.value);
    }
    else if (IS("TMED","TMT")) { SET_TEXT(media_type); }
    else if (IS("TOAL","TOT")) { SET_TEXT(original_title); }
    else if (IS("TOPE","TOE")) { SET_TEXT(original_artist); }
    else if (IS("TORY","TOR")) { SET_TEXT(original_year); }

    /* --- TXXX / TXX --- */
    else if (IS("TXXX","TXX")) {
        if (size < 2) return;
        uint8_t enc = data[0];
        const uint8_t *p = data + 1;
        uint32_t rem = size - 1;
        char *key = NULL;
        uint32_t consumed = read_encoded_str(p, rem, enc, &key);
        if (consumed > rem) { free(key); return; }
        p += consumed; rem -= consumed;

        char *val = NULL;
        if (rem > 0) {
            uint8_t tmp[rem + 1];
            tmp[0] = enc;
            memcpy(tmp + 1, p, rem);
            val = decode_text(tmp, rem + 1);
        } else val = strdup("");

        check_replaygain_txxx(tag, key, val);

        ID3UserText *ut = safe_realloc(tag->user_texts,
            (tag->user_text_count + 1) * sizeof(ID3UserText));
        if (ut) {
            tag->user_texts = ut;
            ID3UserText *entry = &ut[tag->user_text_count++];
            memset(entry, 0, sizeof(*entry));
            if (key) {
                strncpy(entry->key, key, 127); entry->key[127] = '\0';
            }
            entry->value = val; val = NULL;
        }
        free(key); free(val);
    }

    /* URL frames */
    else if ((id[0] == 'W' || (major==2 && id[0]=='W')) &&
             !IS("WXXX","WXX")) {
        /* Standard W-frame: body is URL in Latin-1 */
        char *url = latin1_to_utf8(data, size);
        ID3Url *u = safe_realloc(tag->urls,
            (tag->url_count + 1) * sizeof(ID3Url));
        if (u && url) {
            tag->urls = u;
            ID3Url *entry = &u[tag->url_count++];
            memset(entry, 0, sizeof(*entry));
            strncpy(entry->id, id, 4); entry->id[4] = '\0';
            entry->url = url; url = NULL;
        }
        free(url);
    }
    else if (IS("WXXX","WXX")) {
        if (size < 2) return;
        uint8_t enc = data[0];
        const uint8_t *p = data + 1; uint32_t rem = size - 1;
        char *desc = NULL;
        uint32_t consumed = read_encoded_str(p, rem, enc, &desc);
        if (consumed > rem) { free(desc); return; }
        p += consumed; rem -= consumed;
        char *url = latin1_to_utf8(p, rem); /* URL always Latin-1 */

        ID3Url *u = safe_realloc(tag->urls,
            (tag->url_count + 1) * sizeof(ID3Url));
        if (u) {
            tag->urls = u;
            ID3Url *entry = &u[tag->url_count++];
            memset(entry, 0, sizeof(*entry));
            strncpy(entry->id, id, 4); entry->id[4] = '\0';
            if (desc) { strncpy(entry->description, desc, 127);
                        entry->description[127] = '\0'; }
            entry->url = url; url = NULL;
        }
        free(desc); free(url);
    }

    /* --- COMM / COM --- */
    else if (IS("COMM","COM")) {
        if (size < 5) return;
        uint8_t enc = data[0];
        char lang[4]; memcpy(lang, data + 1, 3); lang[3] = '\0';
        const uint8_t *p = data + 4; uint32_t rem = size - 4;
        char *desc = NULL;
        uint32_t consumed = read_encoded_str(p, rem, enc, &desc);
        if (consumed > rem) { free(desc); return; }
        p += consumed; rem -= consumed;

        uint8_t *tmp = malloc(rem + 1);
        char *text = NULL;
        if (tmp) { tmp[0] = enc; memcpy(tmp+1, p, rem);
                   text = decode_text(tmp, rem+1); free(tmp); }

        ID3Comment *c = safe_realloc(tag->comments,
            (tag->comment_count + 1) * sizeof(ID3Comment));
        if (c) {
            tag->comments = c;
            ID3Comment *entry = &c[tag->comment_count++];
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->lang, lang, 4);
            if (desc) { strncpy(entry->description, desc, 127);
                        entry->description[127] = '\0'; }
            entry->text = text; text = NULL;
        }
        free(desc); free(text);
    }

    /* --- USLT / ULT --- */
    else if (IS("USLT","ULT")) {
        if (size < 5) return;
        uint8_t enc = data[0];
        char lang[4]; memcpy(lang, data + 1, 3); lang[3] = '\0';
        const uint8_t *p = data + 4; uint32_t rem = size - 4;
        char *desc = NULL;
        uint32_t consumed = read_encoded_str(p, rem, enc, &desc);
        if (consumed > rem) { free(desc); return; }
        p += consumed; rem -= consumed;

        uint8_t *tmp = malloc(rem + 1);
        char *text = NULL;
        if (tmp) { tmp[0] = enc; memcpy(tmp+1, p, rem);
                   text = decode_text(tmp, rem+1); free(tmp); }

        ID3Lyrics *l = safe_realloc(tag->lyrics,
            (tag->lyrics_count + 1) * sizeof(ID3Lyrics));
        if (l) {
            tag->lyrics = l;
            ID3Lyrics *entry = &l[tag->lyrics_count++];
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->lang, lang, 4);
            if (desc) { strncpy(entry->description, desc, 127);
                        entry->description[127] = '\0'; }
            entry->text = text; text = NULL;
        }
        free(desc); free(text);
    }

    /* --- SYLT / SLT --- */
    else if (IS("SYLT","SLT")) {
        if (size < 6) return;
        uint8_t enc = data[0];
        char lang[4]; memcpy(lang, data+1, 3); lang[3] = '\0';
        uint8_t ts_fmt  = data[4];
        uint8_t ct      = data[5];
        char *desc = NULL;
        const uint8_t *p = data + 6; uint32_t rem = size - 6;
        uint32_t consumed = read_encoded_str(p, rem, enc, &desc);
        if (consumed <= rem) { p += consumed; rem -= consumed; }

        /* [text\0][timestamp 4 bytes] records */
        ID3SyncLyrics *sl = safe_realloc(tag->sync_lyrics,
            (tag->sync_lyrics_count + 1) * sizeof(ID3SyncLyrics));
        if (!sl) { free(desc); return; }
        tag->sync_lyrics = sl;
        ID3SyncLyrics *entry = &sl[tag->sync_lyrics_count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->lang, lang, 4);
        if (desc) { strncpy(entry->description, desc, 127);
                    entry->description[127] = '\0'; }
        entry->timestamp_format = ts_fmt;
        entry->content_type = ct;

        while (rem >= 5) {
            char *line = NULL;
            uint32_t nc = read_encoded_str(p, rem, enc, &line);
            if (nc > rem) { free(line); break; }
            p += nc; rem -= nc;
            if (rem < 4) { free(line); break; }
            uint32_t ts = be32(p); p += 4; rem -= 4;

            ID3SyltEntry *es = safe_realloc(entry->entries,
                (entry->entry_count + 1) * sizeof(ID3SyltEntry));
            if (es) {
                entry->entries = es;
                es[entry->entry_count].timestamp_ms = ts;
                es[entry->entry_count].text = line; line = NULL;
                entry->entry_count++;
            }
            free(line);
        }
        free(desc);
    }

    /* --- APIC / PIC --- */
    else if (IS("APIC","PIC")) {
        if (size < 4) return;
        uint8_t enc = data[0];
        const uint8_t *p = data + 1; uint32_t rem = size - 1;

        char mime[32] = "image/jpeg";
        uint8_t pic_type = 0;

        if (major == 2) {
            /* v2.2: 3-char format instead of MIME */
            if (rem < 3) return;
            char fmt[4]; memcpy(fmt, p, 3); fmt[3] = '\0';
            p += 3; rem -= 3;
            if      (strcasecmp(fmt, "JPG") == 0) strcpy(mime, "image/jpeg");
            else if (strcasecmp(fmt, "PNG") == 0) strcpy(mime, "image/png");
            else if (strcasecmp(fmt, "GIF") == 0) strcpy(mime, "image/gif");
            else    snprintf(mime, sizeof(mime), "image/%s", fmt);
        } else {
            /* v2.3/v2.4: MIME string null-terminated в Latin-1 */
            uint32_t ml = 0;
            while (ml < rem && p[ml] != 0) ml++;
            uint32_t copy = ml < 31 ? ml : 31;
            memcpy(mime, p, copy); mime[copy] = '\0';
            p += ml + 1; rem -= ml + 1;
        }

        if (rem < 1) return;
        pic_type = *p++; rem--;

        char *desc = NULL;
        uint32_t consumed = read_encoded_str(p, rem, enc, &desc);
        if (consumed > rem) { free(desc); return; }
        p += consumed; rem -= consumed;

        uint8_t *img = malloc(rem);
        if (!img) { free(desc); return; }
        memcpy(img, p, rem);

        ID3Picture *pics = safe_realloc(tag->pictures,
            (tag->picture_count + 1) * sizeof(ID3Picture));
        if (pics) {
            tag->pictures = pics;
            ID3Picture *pic = &pics[tag->picture_count++];
            memset(pic, 0, sizeof(*pic));
            pic->type = (ID3PictureType)pic_type;
            strncpy(pic->mime, mime, 31); pic->mime[31] = '\0';
            if (desc) { strncpy(pic->description, desc, 127);
                        pic->description[127] = '\0'; }
            pic->data = img; img = NULL;
            pic->size = rem;
        }
        free(desc); free(img);
    }

    /* --- GEOB / GEO --- */
    else if (IS("GEOB","GEO")) {
        if (size < 4) return;
        uint8_t enc = data[0];
        const uint8_t *p = data + 1; uint32_t rem = size - 1;
        /* MIME (Latin-1) */
        uint32_t ml = 0;
        while (ml < rem && p[ml]) ml++;
        char mime[64]; uint32_t mc = ml < 63 ? ml : 63;
        memcpy(mime, p, mc); mime[mc] = '\0';
        p += ml + 1; rem -= ml + 1;

        char *filename = NULL, *desc2 = NULL;
        uint32_t nc;
        nc = read_encoded_str(p, rem, enc, &filename);
        if (nc <= rem) { p += nc; rem -= nc; }
        nc = read_encoded_str(p, rem, enc, &desc2);
        if (nc <= rem) { p += nc; rem -= nc; }

        uint8_t *obj = malloc(rem);
        if (!obj) { free(filename); free(desc2); return; }
        memcpy(obj, p, rem);

        ID3Object *objs = safe_realloc(tag->objects,
            (tag->object_count + 1) * sizeof(ID3Object));
        if (objs) {
            tag->objects = objs;
            ID3Object *o = &objs[tag->object_count++];
            memset(o, 0, sizeof(*o));
            strncpy(o->mime, mime, 63); o->mime[63] = '\0';
            if (filename) { strncpy(o->filename, filename, 255);
                            o->filename[255] = '\0'; }
            if (desc2)    { strncpy(o->description, desc2, 127);
                            o->description[127] = '\0'; }
            o->data = obj; o->size = rem; obj = NULL;
        }
        free(filename); free(desc2); free(obj);
    }

    /* CHAP (v2.3/v2.4 only) */
    else if (strcmp(id, "CHAP") == 0) {
        /* element_id\0 start_ms end_ms start_off end_off [subframes] */
        uint32_t ei = 0;
        while (ei < size && data[ei] != 0) ei++;
        if (ei + 17 > size) return;

        ID3Chapter *chs = safe_realloc(tag->chapters,
            (tag->chapter_count + 1) * sizeof(ID3Chapter));
        if (!chs) return;
        tag->chapters = chs;
        ID3Chapter *ch = &chs[tag->chapter_count++];
        memset(ch, 0, sizeof(*ch));
        uint32_t copy = ei < 63 ? ei : 63;
        memcpy(ch->element_id, data, copy); ch->element_id[copy] = '\0';
        const uint8_t *q = data + ei + 1;
        ch->start_ms     = be32(q);   q += 4;
        ch->end_ms       = be32(q);   q += 4;
        ch->start_offset = be32(q);   q += 4;
        ch->end_offset   = be32(q);
    }

    else if (strcmp(id, "RVA2") == 0 || strcmp(id, "RVAD") == 0 ||
             strcmp(id, "RVA") == 0) {
        parse_rva2(data, size, tag);
    }

    /* Unknown frame: store as raw */
    else {
        struct ID3RawFrame *rf = safe_realloc(tag->raw_frames,
            (tag->raw_frame_count + 1) * sizeof(struct ID3RawFrame));
        if (rf) {
            tag->raw_frames = rf;
            struct ID3RawFrame *entry = &rf[tag->raw_frame_count++];
            memset(entry, 0, sizeof(*entry));
            strncpy(entry->id, id, 4); entry->id[4] = '\0';
            entry->data = malloc(size);
            if (entry->data) { memcpy(entry->data, data, size); entry->size = size; }
        }
    }

#undef IS
#undef SET_TEXT
}

/* Parse ID3v2 from buffer (entire tag in memory) */
static ID3Result parse_id3v2_buffer(const uint8_t *buf, uint32_t buf_size,
                                    ID3Tag *tag) {
    if (buf_size < 10) return ID3_ERR_CORRUPT;
    if (buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') return ID3_ERR_CORRUPT;

    uint8_t major    = buf[3];
    uint8_t revision = buf[4];
    uint8_t flags    = buf[5];

    if (major < 2 || major > 4) return ID3_ERR_CORRUPT;

    uint32_t tag_size = synchsafe_decode(&buf[6]);
    if (tag_size + 10 > buf_size) tag_size = buf_size - 10;

    tag->has_v2       = true;
    tag->v2_major     = major;
    tag->v2_revision  = revision;
    tag->v2_tag_size  = tag_size + 10;

    ID3Version new_ver = (major == 2) ? ID3_VERSION_2_2
                       : (major == 3) ? ID3_VERSION_2_3
                                      : ID3_VERSION_2_4;
    if (new_ver > tag->version) tag->version = new_ver;

    bool global_unsync = (flags & 0x80) != 0;

    const uint8_t *body     = buf + 10;
    uint32_t       body_len = tag_size;

    uint8_t *unsync_body = NULL;
    if (global_unsync) {
        uint32_t ud;
        unsync_body = unsync_decode(body, body_len, &ud);
        if (unsync_body) { body = unsync_body; body_len = ud; }
    }

    uint32_t pos = 0;

    /* Extended header */
    if (flags & 0x40) {
        if (pos + 4 > body_len) goto done;
        uint32_t ext_size = (major == 4)
            ? synchsafe_decode(&body[pos])
            : be32(&body[pos]);
        pos += ext_size;
    }

    int fhdr_size = (major == 2) ? 6 : 10;

    while (pos + (uint32_t)fhdr_size <= body_len) {
        if (body[pos] == 0) break;

        char frame_id[5] = {0};
        uint32_t frame_size;
        uint16_t frame_flags = 0;

        if (major == 2) {
            memcpy(frame_id, &body[pos], 3);
            frame_size = be24(&body[pos + 3]);
        } else {
            memcpy(frame_id, &body[pos], 4);
            frame_size = (major == 4)
                ? synchsafe_decode(&body[pos + 4])
                : be32(&body[pos + 4]);
            frame_flags = be16(&body[pos + 8]);
        }

        pos += fhdr_size;
        if (frame_size == 0)       { continue; }
        if (pos + frame_size > body_len) break;

        const uint8_t *fdata = &body[pos];
        uint32_t       fsize = frame_size;

        /* Per-frame unsync (v2.4: frame_flags bit 1) */
        uint8_t *frame_unsync = NULL;
        if (major == 4 && (frame_flags & 0x0002)) {
            uint32_t ud;
            frame_unsync = unsync_decode(fdata, fsize, &ud);
            if (frame_unsync) { fdata = frame_unsync; fsize = ud; }
        }

        /* Skip compressed/encrypted frames (v2.3: 0x0080|0x0040, v2.4: 0x0008|0x0004) */
        bool skip = false;
        if (major == 3 && (frame_flags & (0x0080 | 0x0040))) skip = true;
        if (major == 4 && (frame_flags & (0x0008 | 0x0004))) skip = true;

        if (!skip)
            process_frame(tag, frame_id, fdata, fsize, major);

        free(frame_unsync);
        pos += frame_size;
    }

done:
    free(unsync_body);
    return ID3_OK;
}

/* Parse ID3v1 from last 128 bytes */
static void parse_id3v1(const uint8_t *tail128, ID3Tag *tag) {
    if (tail128[0] != 'T' || tail128[1] != 'A' || tail128[2] != 'G')
        return;

    tag->has_v1 = true;

/* Set only if v2 didn't fill the field */
#define SET_V1(field, offset, len) do { \
    if (!tag->field.value) { \
        uint32_t _n = (len); \
        while (_n > 0 && (tail128[(offset)+_n-1] == 0 || \
                          tail128[(offset)+_n-1] == ' ')) _n--; \
        if (_n > 0) { \
            char *_s = latin1_to_utf8(&tail128[offset], _n); \
            if (_s) textfield_set(&tag->field, _s); \
        } \
    } \
} while(0)

    SET_V1(title,  3,  30);
    SET_V1(artist, 33, 30);
    SET_V1(album,  63, 30);
    SET_V1(year,   93,  4);

    /* ID3v1.1: track in bytes 125-126 */
    bool is_v11 = (tail128[125] == 0 && tail128[126] != 0);
    if (is_v11) {
        tag->version = ID3_VERSION_1_1;
        if (tag->track_number == 0)
            tag->track_number = tail128[126];
    } else {
        if (tag->version < ID3_VERSION_1)
            tag->version = ID3_VERSION_1;
    }

    if (!tag->genre.value) {
        uint8_t gi = tail128[127];
        if (gi < ID3V1_GENRE_COUNT) {
            char *g = strdup(id3v1_genres[gi]);
            if (g) textfield_set(&tag->genre, g);
        }
    }

#undef SET_V1
}

void id3_tag_init(ID3Tag *tag) {
    memset(tag, 0, sizeof(*tag));
}

ID3Result id3_parse_fp(FILE *fp, ID3Tag *tag) {
    if (!fp || !tag) return ID3_ERR_FILE;

    if (fseek(fp, 0, SEEK_END) != 0) return ID3_ERR_FILE;
    long file_size = ftell(fp);
    if (file_size < 0) return ID3_ERR_FILE;
    rewind(fp);

    bool found_any = false;

    {
        uint8_t hdr[10];
        if (fread(hdr, 1, 10, fp) == 10 &&
            hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {

            uint32_t tag_body_size = synchsafe_decode(&hdr[6]);
            uint32_t total = 10 + tag_body_size;

            if (hdr[3] == 4 && (hdr[5] & 0x10)) total += 10;

            if (total <= (uint32_t)file_size) {
                uint8_t *buf = malloc(total);
                if (buf) {
                    rewind(fp);
                    if (fread(buf, 1, total, fp) == total) {
                        ID3Result r = parse_id3v2_buffer(buf, total, tag);
                        if (r == ID3_OK) found_any = true;
                    }
                    free(buf);
                }
            }
        }
    }

    if (file_size >= 10) {
        long footer_offsets[2] = { file_size - 10, file_size - 138 };
        for (int fi = 0; fi < 2; fi++) {
            if (footer_offsets[fi] < 0) continue;
            if (fseek(fp, footer_offsets[fi], SEEK_SET) != 0) continue;
            uint8_t ftr[10];
            if (fread(ftr, 1, 10, fp) != 10) continue;
            if (ftr[0] != '3' || ftr[1] != 'D' || ftr[2] != 'I') continue;
            if (ftr[3] < 2 || ftr[3] > 4) continue;
            uint32_t body_sz = synchsafe_decode(&ftr[6]);
            long tag_start   = footer_offsets[fi] - (long)body_sz - 10;
            if (tag_start < 0) continue;

            uint32_t total = 10 + body_sz + 10; /* header + body + footer */
            uint8_t *buf = malloc(total);
            if (!buf) continue;
            if (fseek(fp, tag_start, SEEK_SET) != 0) { free(buf); continue; }
            if (fread(buf, 1, total, fp) == total &&
                buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
                ID3Result r = parse_id3v2_buffer(buf, total, tag);
                if (r == ID3_OK) found_any = true;
            }
            free(buf);
        }
    }

    if (file_size >= 128) {
        if (fseek(fp, file_size - 128, SEEK_SET) == 0) {
            uint8_t tail[128];
            if (fread(tail, 1, 128, fp) == 128) {
                if (tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G') {
                    parse_id3v1(tail, tag);
                    found_any = true;
                }
            }
        }
    }

    return found_any ? ID3_OK : ID3_ERR_NO_TAG;
}

ID3Result id3_parse_file(const char *path, ID3Tag *tag) {
    if (!path || !tag) return ID3_ERR_FILE;
    FILE *fp = fopen(path, "rb");
    if (!fp) return ID3_ERR_FILE;
    ID3Result r = id3_parse_fp(fp, tag);
    fclose(fp);
    return r;
}

void id3_free(ID3Tag *tag) {
    if (!tag) return;

    textfield_free(&tag->title);        textfield_free(&tag->subtitle);
    textfield_free(&tag->content_group);textfield_free(&tag->artist);
    textfield_free(&tag->album_artist); textfield_free(&tag->conductor);
    textfield_free(&tag->remixer);      textfield_free(&tag->album);
    textfield_free(&tag->track);        textfield_free(&tag->disc);
    textfield_free(&tag->year);         textfield_free(&tag->genre);
    textfield_free(&tag->composer);     textfield_free(&tag->lyricist);
    textfield_free(&tag->publisher);    textfield_free(&tag->copyright);
    textfield_free(&tag->encoded_by);   textfield_free(&tag->encoder_settings);
    textfield_free(&tag->isrc);         textfield_free(&tag->language);
    textfield_free(&tag->key);          textfield_free(&tag->bpm);
    textfield_free(&tag->length_ms);    textfield_free(&tag->media_type);
    textfield_free(&tag->original_title);textfield_free(&tag->original_artist);
    textfield_free(&tag->original_year);

    for (uint32_t i = 0; i < tag->picture_count; i++)
        free(tag->pictures[i].data);
    free(tag->pictures);

    for (uint32_t i = 0; i < tag->comment_count; i++)
        free(tag->comments[i].text);
    free(tag->comments);

    for (uint32_t i = 0; i < tag->lyrics_count; i++)
        free(tag->lyrics[i].text);
    free(tag->lyrics);

    for (uint32_t i = 0; i < tag->user_text_count; i++)
        free(tag->user_texts[i].value);
    free(tag->user_texts);

    for (uint32_t i = 0; i < tag->url_count; i++)
        free(tag->urls[i].url);
    free(tag->urls);

    for (uint32_t i = 0; i < tag->sync_lyrics_count; i++) {
        for (uint32_t j = 0; j < tag->sync_lyrics[i].entry_count; j++)
            free(tag->sync_lyrics[i].entries[j].text);
        free(tag->sync_lyrics[i].entries);
    }
    free(tag->sync_lyrics);

    free(tag->chapters);

    for (uint32_t i = 0; i < tag->object_count; i++)
        free(tag->objects[i].data);
    free(tag->objects);

    for (uint32_t i = 0; i < tag->raw_frame_count; i++)
        free(tag->raw_frames[i].data);
    free(tag->raw_frames);

    memset(tag, 0, sizeof(*tag));
}

const char *id3_version_str(const ID3Version v) {
    switch (v) {
        case ID3_VERSION_1:   return "ID3v1.0";
        case ID3_VERSION_1_1: return "ID3v1.1";
        case ID3_VERSION_2_2: return "ID3v2.2";
        case ID3_VERSION_2_3: return "ID3v2.3";
        case ID3_VERSION_2_4: return "ID3v2.4";
        default:              return "None";
    }
}

const char *id3_picture_type_str(const ID3PictureType t) {
    switch (t) {
        case ID3_PIC_OTHER:            return "Other";
        case ID3_PIC_FILE_ICON:        return "File icon";
        case ID3_PIC_OTHER_ICON:       return "Other icon";
        case ID3_PIC_FRONT_COVER:      return "Front cover";
        case ID3_PIC_BACK_COVER:       return "Back cover";
        case ID3_PIC_LEAFLET:          return "Leaflet";
        case ID3_PIC_MEDIA:            return "Media";
        case ID3_PIC_LEAD_ARTIST:      return "Lead artist";
        case ID3_PIC_ARTIST:           return "Artist";
        case ID3_PIC_CONDUCTOR:        return "Conductor";
        case ID3_PIC_BAND:             return "Band/Orchestra";
        case ID3_PIC_COMPOSER:         return "Composer";
        case ID3_PIC_LYRICIST:         return "Lyricist";
        case ID3_PIC_RECORDING_LOCATION: return "Recording location";
        case ID3_PIC_DURING_RECORDING: return "During recording";
        case ID3_PIC_DURING_PERFORMANCE: return "During performance";
        case ID3_PIC_VIDEO_CAPTURE:    return "Video capture";
        case ID3_PIC_ILLUSTRATION:     return "Illustration";
        case ID3_PIC_BAND_LOGOTYPE:    return "Band logotype";
        case ID3_PIC_PUBLISHER_LOGOTYPE: return "Publisher logotype";
        default:                       return "Unknown";
    }
}

const char * id3_text(const ID3TextField *f) {
    return f->value;
}

/* Debug dump */
#define PRINT_FIELD(label, field) do { \
    if ((field).value) { \
        fprintf(out, "  %-20s %s\n", label ":", (field).value); \
        if ((field).count > 1 && (field).values) { \
            for (uint32_t _i = 1; _i < (field).count; _i++) \
                fprintf(out, "  %-20s %s\n", "", (field).values[_i]); \
        } \
    } \
} while(0)

void id3_dump(const ID3Tag *tag, FILE *out) {
    if (!tag || !out) return;
    fprintf(out, "=== ID3 Tag Dump ===\n");
    fprintf(out, "  %-20s %s\n", "Version:", id3_version_str(tag->version));
    if (tag->has_v2)
        fprintf(out, "  %-20s 2.%d.%d  (%" PRIu32 " bytes)\n",
                "ID3v2:", tag->v2_major, tag->v2_revision, tag->v2_tag_size);
    fprintf(out, "\n--- Core fields ---\n");
    PRINT_FIELD("Title",          tag->title);
    PRINT_FIELD("Subtitle",       tag->subtitle);
    PRINT_FIELD("Artist",         tag->artist);
    PRINT_FIELD("Album Artist",   tag->album_artist);
    PRINT_FIELD("Album",          tag->album);
    if (tag->track_number)
        fprintf(out, "  %-20s %" PRIu32 "%s\n", "Track:",
                tag->track_number,
                tag->track_total ? (char[16]){0} : "");
    if (tag->track_total)
        fprintf(out, "  %-20s %" PRIu32 "\n", "Track total:", tag->track_total);
    if (tag->disc_number)
        fprintf(out, "  %-20s %" PRIu32 "/%" PRIu32 "\n", "Disc:",
                tag->disc_number, tag->disc_total);
    PRINT_FIELD("Year",           tag->year);
    PRINT_FIELD("Genre",          tag->genre);
    PRINT_FIELD("Composer",       tag->composer);
    PRINT_FIELD("Publisher",      tag->publisher);
    PRINT_FIELD("Copyright",      tag->copyright);
    PRINT_FIELD("Language",       tag->language);
    PRINT_FIELD("Key",            tag->key);
    if (tag->bpm_value)
        fprintf(out, "  %-20s %" PRIu32 "\n", "BPM:", tag->bpm_value);
    if (tag->length_ms_value)
        fprintf(out, "  %-20s %" PRIu32 " ms (%" PRIu32 ":%02" PRIu32 ")\n", "Length:",
                tag->length_ms_value,
                tag->length_ms_value / 60000,
                (tag->length_ms_value / 1000) % 60);
    PRINT_FIELD("ISRC",           tag->isrc);
    PRINT_FIELD("Encoded by",     tag->encoded_by);
    PRINT_FIELD("Encoder",        tag->encoder_settings);

    if (tag->picture_count) {
        fprintf(out, "\n--- Pictures (%" PRIu32 ") ---\n", tag->picture_count);
        for (uint32_t i = 0; i < tag->picture_count; i++) {
            const ID3Picture *p = &tag->pictures[i];
            fprintf(out, "  [%" PRIu32 "] %-16s %s  %" PRIu32 " bytes  \"%s\"\n",
                    i, id3_picture_type_str(p->type),
                    p->mime, p->size, p->description);
        }
    }
    if (tag->comment_count) {
        fprintf(out, "\n--- Comments (%" PRIu32 ") ---\n", tag->comment_count);
        for (uint32_t i = 0; i < tag->comment_count; i++) {
            const ID3Comment *c = &tag->comments[i];
            fprintf(out, "  [%" PRIu32 "] [%s] \"%s\": %s\n",
                    i, c->lang, c->description, c->text ? c->text : "");
        }
    }
    if (tag->lyrics_count) {
        fprintf(out, "\n--- Lyrics (%" PRIu32 ") ---\n", tag->lyrics_count);
        for (uint32_t i = 0; i < tag->lyrics_count; i++) {
            const ID3Lyrics *l = &tag->lyrics[i];
            size_t tlen = l->text ? strlen(l->text) : 0;
            fprintf(out, "  [%" PRIu32 "] [%s] \"%s\": %zu chars\n",
                    i, l->lang, l->description, tlen);
        }
    }
    if (tag->user_text_count) {
        fprintf(out, "\n--- User Text (TXXX) (%" PRIu32 ") ---\n", tag->user_text_count);
        for (uint32_t i = 0; i < tag->user_text_count; i++)
            fprintf(out, "  %-30s %s\n",
                    tag->user_texts[i].key,
                    tag->user_texts[i].value ? tag->user_texts[i].value : "");
    }
    if (tag->url_count) {
        fprintf(out, "\n--- URLs (%" PRIu32 ") ---\n", tag->url_count);
        for (uint32_t i = 0; i < tag->url_count; i++)
            fprintf(out, "  [%s] %-20s %s\n",
                    tag->urls[i].id, tag->urls[i].description,
                    tag->urls[i].url ? tag->urls[i].url : "");
    }
    if (tag->has_replay_gain) {
        fprintf(out, "\n--- ReplayGain ---\n");
        fprintf(out, "  Track gain: %+.2f dB\n", tag->replay_gain.track_gain_db);
        if (tag->replay_gain.has_album)
            fprintf(out, "  Album gain: %+.2f dB\n", tag->replay_gain.album_gain_db);
    }
    if (tag->chapter_count) {
        fprintf(out, "\n--- Chapters (%" PRIu32 ") ---\n", tag->chapter_count);
        for (uint32_t i = 0; i < tag->chapter_count; i++) {
            const ID3Chapter *ch = &tag->chapters[i];
            fprintf(out, "  [%" PRIu32 "] \"%s\"  %" PRIu32 " ms – %" PRIu32 " ms\n",
                    i, ch->element_id, ch->start_ms, ch->end_ms);
        }
    }
    if (tag->raw_frame_count) {
        fprintf(out, "\n--- Unknown frames (%" PRIu32 ") ---\n", tag->raw_frame_count);
        for (uint32_t i = 0; i < tag->raw_frame_count; i++)
            fprintf(out, "  %s  %" PRIu32 " bytes\n",
                    tag->raw_frames[i].id, tag->raw_frames[i].size);
    }
    fprintf(out, "===================\n");
}