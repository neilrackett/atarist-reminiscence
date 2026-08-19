/* ipf2adf: decode a CAPS IPF image's AmigaDOS tracks into an ADF.
 * Links against capsimg; MFM sector decoding done here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef int32_t  SDWORD;
typedef uint32_t UDWORD;
typedef uint8_t  UBYTE;
typedef uint8_t *PUBYTE;
typedef uint32_t *PUDWORD;
typedef void *PVOID;
typedef char *PCHAR;

#pragma pack(push, 1)
struct CapsTrackInfoT2 {
    UDWORD type, cylinder, head, sectorcnt, sectorsize;
    PUBYTE trackbuf;
    UDWORD tracklen, timelen;
    PUDWORD timebuf;
    SDWORD overlap;
    UDWORD startbit, wseed, weakcnt;
};
#pragma pack(pop)

extern SDWORD CAPSInit(void);
extern SDWORD CAPSExit(void);
extern SDWORD CAPSAddImage(void);
extern SDWORD CAPSRemImage(SDWORD id);
extern SDWORD CAPSLockImage(SDWORD id, PCHAR name);
extern SDWORD CAPSUnlockImage(SDWORD id);
extern SDWORD CAPSLoadImage(SDWORD id, UDWORD flag);
extern SDWORD CAPSLockTrack(PVOID pi, SDWORD id, UDWORD cyl, UDWORD head, UDWORD flag);
extern SDWORD CAPSUnlockTrack(SDWORD id, UDWORD cyl, UDWORD head);
extern SDWORD CAPSUnlockAllTracks(SDWORD id);

#define DI_LOCK_INDEX   (1<<0)
#define DI_LOCK_ALIGN   (1<<1)
#define DI_LOCK_DENVAR  (1<<2)
#define DI_LOCK_TYPE    (1<<9)
#define DI_LOCK_DENALT  (1<<10)
#define DI_LOCK_UPDATEFD (1<<8)
#define DI_LOCK_OVLBIT  (1<<11)
#define DI_LOCK_TRKBIT  (1<<12)

#define CYLS 80
#define HEADS 2
#define SECS 11
#define SECSIZE 512

/* read bit 'pos' (modulo len) from track bitstream */
static inline int getbit(const uint8_t *buf, uint32_t bitlen, uint32_t pos) {
    pos %= bitlen;
    return (buf[pos >> 3] >> (7 - (pos & 7))) & 1;
}

static uint32_t get32(const uint8_t *buf, uint32_t bitlen, uint32_t pos) {
    uint32_t v = 0;
    for (int i = 0; i < 32; i++)
        v = (v << 1) | getbit(buf, bitlen, pos + i);
    return v;
}

/* decode odd/even MFM longword pair at bit position pos (odd first),
 * words separated by 'gap' bits (gap = 32 for longword interleave) */
static uint32_t mfm_long(const uint8_t *buf, uint32_t bitlen, uint32_t pos, uint32_t gap) {
    uint32_t odd = get32(buf, bitlen, pos) & 0x55555555;
    uint32_t even = get32(buf, bitlen, pos + gap) & 0x55555555;
    return (odd << 1) | even;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s in.ipf out.adf\n", argv[0]);
        return 2;
    }
    if (CAPSInit() != 0) { fprintf(stderr, "CAPSInit failed\n"); return 1; }
    SDWORD id = CAPSAddImage();
    if (CAPSLockImage(id, argv[1]) != 0) { fprintf(stderr, "cannot lock %s\n", argv[1]); return 1; }
    if (CAPSLoadImage(id, DI_LOCK_DENALT|DI_LOCK_DENVAR|DI_LOCK_UPDATEFD) != 0) {
        fprintf(stderr, "CAPSLoadImage failed\n"); return 1;
    }

    struct { UDWORD type, release, revision, mincylinder, maxcylinder, minhead, maxhead; } info[4];
    memset(info, 0, sizeof(info));
    extern SDWORD CAPSGetImageInfo(PVOID pi, SDWORD id);
    if (CAPSGetImageInfo(&info[0], id) == 0)
        fprintf(stderr, "imageinfo: type=%u release=%u rev=%u cyl=%u..%u head=%u..%u\n",
                info[0].type, info[0].release, info[0].revision,
                info[0].mincylinder, info[0].maxcylinder, info[0].minhead, info[0].maxhead);
    static uint8_t adf[CYLS * HEADS * SECS * SECSIZE];
    memset(adf, 0, sizeof(adf));
    int missing = 0;

    for (int cyl = 0; cyl < CYLS; cyl++) {
        for (int head = 0; head < HEADS; head++) {
            struct CapsTrackInfoT2 ti;
            memset(&ti, 0, sizeof(ti));
            ti.type = 1;
            SDWORD lr = CAPSLockTrack(&ti, id, cyl, head, DI_LOCK_DENALT|DI_LOCK_DENVAR|DI_LOCK_UPDATEFD|DI_LOCK_TYPE);
            if (lr != 0) {
                fprintf(stderr, "lock failed c%d h%d rc=%d\n", cyl, head, lr);
                missing += SECS;
                continue;
            }
            uint32_t bitlen = ti.tracklen * 8; /* tracklen is in bytes here */
            const uint8_t *buf = ti.trackbuf;
            if (!buf || bitlen < 64) {
                fprintf(stderr, "track c%d h%d: empty (buf=%p len=%u type=%u)\n",
                        cyl, head, (void *)buf, ti.tracklen, ti.type);
                missing += SECS;
                CAPSUnlockTrack(id, cyl, head);
                continue;
            }
            int found = 0;
            uint16_t have = 0; /* bitmask of sectors found */
            int tracknum = cyl * 2 + head;
            /* scan for AmigaDOS sector sync 0x4489 0x4489 */
            for (uint32_t pos = 0; pos + 64 < bitlen + 1088 * 16 && found < SECS; pos++) {
                if (get32(buf, bitlen, pos) != 0x44894489u)
                    continue;
                uint32_t p = pos + 32;
                uint32_t info = mfm_long(buf, bitlen, p, 32);
                /* info: 0xFF track sector sectors_to_gap */
                if (((info >> 24) & 0xFF) != 0xFF) continue;
                int trk = (info >> 16) & 0xFF;
                int sec = (info >> 8) & 0xFF;
                if (trk != tracknum || sec >= SECS) continue;
                /* header checksum over info+label (10 longs) */
                uint32_t hsum = 0;
                for (int i = 0; i < 10; i++)
                    hsum ^= get32(buf, bitlen, p + i * 32) & 0x55555555;
                uint32_t hchk = mfm_long(buf, bitlen, p + 320, 32);
                if ((hsum & 0x55555555) != (hchk & 0x55555555)) continue;
                /* layout after sync: info(2 longs mfm), label(8 longs mfm),
                   hdrchk(2), datachk(2), data(256 longs odd then 256 longs even) */
                uint32_t data_off = p + (2 + 8 + 2 + 2) * 32;
                uint32_t dsum = 0;
                uint8_t secbuf[SECSIZE];
                for (int i = 0; i < 128; i++) {
                    uint32_t v = mfm_long(buf, bitlen, data_off + i * 32, 128 * 32);
                    secbuf[i * 4 + 0] = v >> 24;
                    secbuf[i * 4 + 1] = v >> 16;
                    secbuf[i * 4 + 2] = v >> 8;
                    secbuf[i * 4 + 3] = v;
                }
                for (int i = 0; i < 256; i++)
                    dsum ^= get32(buf, bitlen, data_off + i * 32) & 0x55555555;
                uint32_t dchk2 = mfm_long(buf, bitlen, p + (2 + 8 + 2) * 32, 32);
                if ((dsum & 0x55555555) != (dchk2 & 0x55555555)) continue;
                if (!(have & (1 << sec))) {
                    have |= 1 << sec;
                    found++;
                    memcpy(adf + (tracknum * SECS + sec) * SECSIZE, secbuf, SECSIZE);
                }
                pos += 32; /* keep scanning */
            }
            if (found != SECS) {
                fprintf(stderr, "track %d (c%d h%d): only %d/%d sectors (type=%u len=%u)\n",
                        tracknum, cyl, head, found, SECS, ti.type & 0xff, ti.tracklen);
                missing += SECS - found;
            }
            CAPSUnlockTrack(id, cyl, head);
        }
    }
    CAPSUnlockImage(id);
    CAPSRemImage(id);
    CAPSExit();

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    fwrite(adf, 1, sizeof(adf), f);
    fclose(f);
    printf("%s: wrote %s, %d sectors missing\n", argv[1], argv[2], missing);
    return missing ? 3 : 0;
}
