#include "gifdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#pragma warning(disable : 4996)
#include <io.h>
#else
#include <unistd.h>
#endif

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

#define GIF_OFFSET_HEADER       0
#define GIF_OFFSET_VERSION      3
#define GIF_OFFSET_WIDTH        6
#define GIF_OFFSET_HEIGHT       8
#define GIF_OFFSET_FDSZ         10
#define GIF_OFFSET_BGC          11
#define GIF_OFFSET_AR           12
#define GIF_OFFSET_GCT          13

typedef struct Entry {
    uint16_t length;
    uint16_t prefix;
    uint8_t  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry *entries;
} Table;


static void gif_read_data(gd_GIF *gif, uint8_t *dst, uint32_t len)
{
    memcpy(dst, &gif->gif_data[gif->gif_data_idx], len);
    gif->gif_data_idx += len;
}

static void gif_skip_data(gd_GIF *gif, uint32_t n)
{
    gif->gif_data_idx += n;
}

gd_GIF *
gd_open_gif(const char *fname)
{
    gd_GIF *gif = NULL;
    uint32_t size;

    struct stat st;
    if (stat(fname, &st) != 0){
        fprintf(stderr, "Failt to get file size\n");
        return NULL;
    }

    /* Create gd_GIF Structure. */
    gif = calloc(1, sizeof(gd_GIF));

    if(!gif){
        fprintf(stderr, "Fail to allocate gif structure\n");
        return NULL;
    }

    size = st.st_size;
    gif->gif_data = (uint8_t*)malloc(size);

    /* Read file to memory */
    FILE *fp = fopen(fname, "r");
    
    if (fp == NULL){
        return NULL;
    }  
    
    fread(gif->gif_data, 1, size, fp);
    fclose(fp);

    int res = gd_init_gif(gif, gif->gif_data);

    if(res){
        free(gif->gif_data);
        free(gif);
        return NULL;
    }
    
    return gif;
}

int gd_init_gif(gd_GIF *gif, uint8_t *raw)
{
    uint8_t sigver[3];
    uint8_t fdsz, aspect;

    /* Header */
    memcpy(sigver, &raw[GIF_OFFSET_HEADER], 3);
    if (memcmp(sigver, "GIF", 3) != 0) {
        fprintf(stderr, "invalid signature\n");
        return -1;
    }

    /* Version */
    memcpy(sigver,&raw[GIF_OFFSET_VERSION], 3);
    if (memcmp(sigver, "89a", 3) != 0) {
        fprintf(stderr, "invalid version\n");
        return -1;
    }

    /* Width x Height */
    memcpy(&gif->width, &raw[GIF_OFFSET_WIDTH], 2);
    memcpy(&gif->height, &raw[GIF_OFFSET_HEIGHT], 2);

    /* FDSZ */
    memcpy(&fdsz, &raw[GIF_OFFSET_FDSZ], 1);
    
    /* Presence of GCT */
    if (!(fdsz & 0x80)) {
        fprintf(stderr, "no global color table\n");
        return -1;
    }

    /* Color Space's Depth */
    gif->depth = ((fdsz >> 4) & 7) + 1;

    /* Ignore Sort Flag. */
    /* GCT Size */
    gif->gct.size = 1 << ((fdsz & 0x07) + 1);

    /* Background Color Index */
    memcpy(&gif->bgindex, &raw[GIF_OFFSET_BGC], 1);

    /* Aspect Ratio */
    memcpy(&aspect, &raw[GIF_OFFSET_AR], 1);

    /* Read GCT */
    memcpy(gif->gct.colors, &raw[GIF_OFFSET_GCT], 3 * gif->gct.size);
    
    gif->gif_data = raw;
    gif->palette = &gif->gct;
    
    gif->canvas = (uint8_t*)malloc(4 * gif->width * gif->height);
    gif->frame = &gif->canvas[3 * gif->width * gif->height];

    if (gif->bgindex){
        memset(gif->frame, gif->bgindex, gif->width * gif->height);
    }
    
    uint8_t *bgcolor = &gif->palette->colors[gif->bgindex * 3];
    
    if (bgcolor[0] || bgcolor[1] || bgcolor [2]){
        for (int i = 0; i < gif->width * gif->height; i++){
            memcpy(&gif->canvas[i * 3], bgcolor, 3);
        }
    }

    gif->anim_start = GIF_OFFSET_GCT + 3 * gif->gct.size;
    gif->gif_data_idx = gif->anim_start;

    return 0;   
}


static void
discard_sub_blocks(gd_GIF *gif)
{
    uint8_t size;

    do {
        gif_read_data(gif, &size, 1);
        gif_skip_data(gif, size);
    } while (size);
}

static void
read_plain_text_ext(gd_GIF *gif)
{
    if (gif->plain_text) {
        uint16_t tx, ty, tw, th;
        uint8_t cw, ch, fg, bg;
        off_t sub_block;
        /* block size = 12 */
        gif_skip_data(gif, 1);
        gif_read_data(gif, (uint8_t*)&tx, 2);
        gif_read_data(gif, (uint8_t*)&ty, 2);
        gif_read_data(gif, (uint8_t*)&tw, 2);
        gif_read_data(gif, (uint8_t*)&th, 2);
        gif_read_data(gif, &cw, 1);
        gif_read_data(gif, &ch, 1);
        gif_read_data(gif, &fg, 1);
        gif_read_data(gif, &bg, 1);        
        sub_block = gif->gif_data_idx;
        gif->plain_text(gif, tx, ty, tw, th, cw, ch, fg, bg);
        gif->gif_data_idx = sub_block;
    } else {
        /* Discard plain text metadata. */
        gif_skip_data(gif, 13);
    }
    /* Discard plain text sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_graphic_control_ext(gd_GIF *gif)
{
    uint8_t rdit;

    /* Discard block size (always 0x04). */
    gif_skip_data(gif, 1);
    gif_read_data(gif, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif_read_data(gif, (uint8_t*)&gif->gce.delay, 2);
    gif_read_data(gif, &gif->gce.tindex, 1);
    /* Skip block terminator. */
    gif_skip_data(gif, 1);
}

static void
read_comment_ext(gd_GIF *gif)
{
    if (gif->comment) {
        uint32_t sub_block = gif->gif_data_idx;
        gif->comment(gif);
        gif->gif_data_idx = sub_block;
    }
    /* Discard comment sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_application_ext(gd_GIF *gif)
{
    char app_id[8];
    char app_auth_code[3];

    /* Discard block size (always 0x0B). */
    gif_skip_data(gif, 1);
    /* Application Identifier. */
    gif_read_data(gif, app_id, 8);
    /* Application Authentication Code. */
    gif_read_data(gif, app_auth_code, 3);

    if (!strncmp(app_id, "NETSCAPE", sizeof(app_id))) {
        /* Discard block size (0x03) and constant byte (0x01). */
        gif_skip_data(gif, 2);
        gif_read_data(gif, (uint8_t*)&gif->loop_count, 2);
        /* Skip block terminator. */
        gif_skip_data(gif, 1);
    } else if (gif->application) {
        uint32_t sub_block = gif->gif_data_idx;
        gif->application(gif, app_id, app_auth_code);
        gif->gif_data_idx = sub_block;
        discard_sub_blocks(gif);
    } else {
        discard_sub_blocks(gif);
    }
}

static void
read_ext(gd_GIF *gif)
{
    uint8_t label;

    gif_read_data(gif, &label, 1);

    switch (label) {
    case 0x01:
        read_plain_text_ext(gif);
        break;
    case 0xF9:
        read_graphic_control_ext(gif);
        break;
    case 0xFE:
        read_comment_ext(gif);
        break;
    case 0xFF:
        read_application_ext(gif);
        break;
    default:
        fprintf(stderr, "unknown extension: %02X\n", label);
    }
}

static Table *
new_table(int key_size)
{
    int key;
    int init_bulk = MAX(1 << (key_size + 1), 0x100);
    Table *table = malloc(sizeof(*table) + sizeof(Entry) * init_bulk);
    if (table) {
        table->bulk = init_bulk;
        table->nentries = (1 << key_size) + 2;
        table->entries = (Entry *) &table[1];
        for (key = 0; key < (1 << key_size); key++)
            table->entries[key] = (Entry) {1, 0xFFF, key};
    }
    return table;
}

/* Add table entry. Return value:
 *  0 on success
 *  +1 if key size must be incremented after this addition
 *  -1 if could not realloc table */
static int
add_entry(Table **tablep, uint16_t length, uint16_t prefix, uint8_t suffix)
{
    Table *table = *tablep;
    if (table->nentries == table->bulk) {
        table->bulk *= 2;
        table = realloc(table, sizeof(*table) + sizeof(Entry) * table->bulk);
        if (!table) return -1;
        table->entries = (Entry *) &table[1];
        *tablep = table;
    }
    table->entries[table->nentries] = (Entry) {length, prefix, suffix};
    table->nentries++;
    if ((table->nentries & (table->nentries - 1)) == 0)
        return 1;
    return 0;
}

static uint16_t
get_key(gd_GIF *gif, int key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte)
{
    int bits_read;
    int rpad;
    int frag_size;
    uint16_t key;

    key = 0;
    for (bits_read = 0; bits_read < key_size; bits_read += frag_size) {
        rpad = (*shift + bits_read) % 8;
        if (rpad == 0) {
            /* Update byte. */
            if (*sub_len == 0) {
                gif_read_data(gif, sub_len, 1); /* Must be nonzero! */
                if (*sub_len == 0)
                    return 0x1000;
            }
            gif_read_data(gif, byte, 1);
            (*sub_len)--;
        }
        frag_size = MIN(key_size - bits_read, 8 - rpad);
        key |= ((uint16_t) ((*byte) >> rpad)) << bits_read;
    }
    /* Clear extra bits to the left. */
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
}

/* Compute output index of y-th input line, in frame of height h. */
static int
interlaced_line_index(int h, int y)
{
    int p; /* number of lines in current pass */

    p = (h - 1) / 8 + 1;
    if (y < p) /* pass 1 */
        return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if (y < p) /* pass 2 */
        return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if (y < p) /* pass 3 */
        return y * 4 + 2;
    y -= p;
    /* pass 4 */
    return y * 2 + 1;
}

/* Decompress image pixels.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image_data(gd_GIF *gif, int interlace)
{
    uint8_t sub_len, shift, byte;
    int init_key_size, key_size, table_is_full;
    int frm_off, frm_size, str_len, i, p, x, y;
    uint16_t key, clear, stop;
    int ret;
    Table *table;
	Entry entry = { 0,0 };
    off_t start, end;

    gif_read_data(gif, &byte, 1);
    key_size = (int) byte;
    start = gif->gif_data_idx;
    discard_sub_blocks(gif);
    end = gif->gif_data_idx;
    gif->gif_data_idx = start;
    clear = 1 << key_size;
    stop = clear + 1;
    table = new_table(key_size);
    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte); /* clear code */
    frm_off = 0;
    ret = 0;
    frm_size = gif->fw*gif->fh;
    while (frm_off < frm_size) {
		if (key == clear) {
            key_size = init_key_size;
            table->nentries = (1 << (key_size - 1)) + 2;
            table_is_full = 0;
        } else if (!table_is_full) {
            ret = add_entry(&table, str_len + 1, key, entry.suffix);
            if (ret == -1) {
                free(table);
                return -1;
            }
            if (table->nentries == 0x1000) {
                ret = 0;
                table_is_full = 1;
            }
        }
        key = get_key(gif, key_size, &sub_len, &shift, &byte);
        if (key == clear) continue;
        if (key == stop || key == 0x1000) break;
        if (ret == 1) key_size++;
        entry = table->entries[key];
        str_len = entry.length;
        for (i = 0; i < str_len; i++) {
            p = frm_off + entry.length - 1;
            x = p % gif->fw;
            y = p / gif->fw;
            if (interlace)
                y = interlaced_line_index((int) gif->fh, y);
            gif->frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;
            if (entry.prefix == 0xFFF)
                break;
			else {
				if (entry.prefix == 0xcdcd) {
					entry.prefix = 0;
				}
				entry = table->entries[entry.prefix];
			}
        }
        frm_off += str_len;
        if (key < table->nentries - 1 && !table_is_full)
            table->entries[table->nentries - 1].suffix = entry.suffix;
    }
    free(table);
    if (key == stop)
        gif_read_data(gif, &sub_len, 1); /* Must be zero! */
    gif->gif_data_idx = end;
    return 0;
}

/* Read image.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image(gd_GIF *gif)
{
    uint8_t fisrz;
    int interlace;

    /* Image Descriptor. */
    gif_read_data(gif, (uint8_t*)&gif->fx,2);
    gif_read_data(gif, (uint8_t*)&gif->fy,2);
    gif_read_data(gif, (uint8_t*)&gif->fw,2);
    gif_read_data(gif, (uint8_t*)&gif->fh,2);
    gif_read_data(gif, &fisrz, 1);
    interlace = fisrz & 0x40;
    /* Ignore Sort Flag. */
    /* Local Color Table? */
    if (fisrz & 0x80) {
        /* Read LCT */
        gif->lct.size = 1 << ((fisrz & 0x07) + 1);
        gif_read_data(gif, gif->lct.colors, 3 * gif->lct.size);
        gif->palette = &gif->lct;
    } else
        gif->palette = &gif->gct;
    /* Image Data. */
    return read_image_data(gif, interlace);
}

static void
render_frame_rect(gd_GIF *gif, uint8_t *buffer)
{
    int i, j, k;
    uint8_t index, *color;
    i = gif->fy * gif->width + gif->fx;
    for (j = 0; j < gif->fh; j++) {
        for (k = 0; k < gif->fw; k++) {
            index = gif->frame[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index*3];
            if (!gif->gce.transparency || index != gif->gce.tindex)
                memcpy(&buffer[(i+k)*3], color, 3);
        }
        i += gif->width;
    }
}

static void
dispose(gd_GIF *gif)
{
    int i, j, k;
    uint8_t *bgcolor;
    switch (gif->gce.disposal) {
        case 2: /* Restore to background color. */
            bgcolor = &gif->palette->colors[gif->bgindex*3];
            i = gif->fy * gif->width + gif->fx;
            for (j = 0; j < gif->fh; j++) {
                for (k = 0; k < gif->fw; k++)
                    memcpy(&gif->canvas[(i+k)*3], bgcolor, 3);
                i += gif->width;
            }
            break;
            
        case 3: /* Restore to previous, i.e., don't update canvas.*/
            break;

        default:
            /* Add frame non-transparent pixels to canvas. */
            render_frame_rect(gif, gif->canvas);
    }
}

/* Return 1 if got a frame; 0 if got GIF trailer; -1 if error. */
int
gd_get_frame(gd_GIF *gif)
{
    char sep;

    dispose(gif);
    
    gif_read_data(gif, (uint8_t*)&sep, 1);

    while (sep != ',') {
        if (sep == ';'){
            return 0;
        }

        if (sep == '!'){
            read_ext(gif);
        }else{
            return -1;
        }        
        
        gif_read_data(gif, (uint8_t*)&sep, 1);
    }

    if (read_image(gif) == -1){
        return -1;
    }
    
    return 1;
}

void
gd_render_frame(gd_GIF *gif, uint8_t *buffer)
{
    memcpy(buffer, gif->canvas, gif->width * gif->height * 3);
    render_frame_rect(gif, buffer);
}

int
gd_is_bgcolor(gd_GIF *gif, uint8_t color[3])
{
    return !memcmp(&gif->palette->colors[gif->bgindex*3], color, 3);
}

void
gd_rewind(gd_GIF *gif)
{
    gif->gif_data_idx = gif->anim_start;
}

void
gd_close_gif(gd_GIF *gif)
{
    //close(gif->fd);
    free(gif->canvas);
    free(gif->gif_data);
    free(gif);
}
