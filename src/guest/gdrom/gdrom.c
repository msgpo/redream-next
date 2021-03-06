#include "guest/gdrom/gdrom.h"
#include "core/math.h"
#include "core/string.h"
#include "guest/dreamcast.h"
#include "guest/gdrom/gdrom_replies.inc"
#include "guest/gdrom/gdrom_types.h"
#include "guest/holly/holly.h"

#if 0
#define LOG_GDROM LOG_INFO
#else
#define LOG_GDROM(...)
#endif

/* internal gdrom state machine */
enum gd_event {
  EVENT_ATA_CMD,
  EVENT_PIO_WRITE,
  EVENT_SPI_CMD,
  EVENT_PIO_READ,
  EVENT_SPI_DATA,
  MAX_EVENTS,
};

enum gd_state {
  STATE_READ_ATA_CMD,
  STATE_READ_ATA_DATA,
  STATE_READ_SPI_DATA,
  STATE_WRITE_SPI_DATA,
  STATE_WRITE_DMA_DATA,
  MAX_STATES,
};

typedef void (*gd_event_cb)(struct gdrom *, int);

static void gdrom_event(struct gdrom *gd, enum gd_event ev, int arg);
static void gdrom_ata_cmd(struct gdrom *gd, int arg);
static void gdrom_pio_write(struct gdrom *gd, int arg);
static void gdrom_spi_cmd(struct gdrom *gd, int arg);
static void gdrom_pio_read(struct gdrom *gd, int arg);
static void gdrom_spi_data(struct gdrom *gd, int arg);

/* clang-format off */
gd_event_cb gd_transitions[MAX_STATES][MAX_EVENTS] = {
  { &gdrom_ata_cmd, NULL,             NULL,           NULL,            NULL,            },
  { &gdrom_ata_cmd, &gdrom_pio_write, &gdrom_spi_cmd, NULL,            NULL,            },
  { &gdrom_ata_cmd, &gdrom_pio_write, NULL,           NULL,            &gdrom_spi_data, },
  { &gdrom_ata_cmd, NULL,             NULL,           &gdrom_pio_read, NULL,            },
  { &gdrom_ata_cmd, NULL,             NULL,           &gdrom_pio_read, NULL,            },
};
/* clang-format on */

struct gdrom {
  struct device;

  enum gd_state state;
  struct gd_hw_info hw_info;
  struct disc *disc;

  /* internal registers */
  union gd_error error;
  union gd_features features;
  union gd_intreason ireason;
  union gd_sectnum sectnum;
  union gd_bytect byte_count;
  union gd_status status;

  /* cdread state */
  int cdr_dma;
  int cdr_secfmt;
  int cdr_secmask;
  int cdr_first_sector;
  int cdr_num_sectors;

  /* pio state */
  uint8_t pio_buffer[0x10000];
  int pio_head;
  int pio_size;
  int pio_offset;

  /* dma state */
  uint8_t dma_buffer[0x10000];
  int dma_head;
  int dma_size;
};

static int gdrom_get_fad(uint8_t a, uint8_t b, uint8_t c, int msf) {
  if (msf) {
    /* MSF mode
       Byte 2 - Start time: minutes (binary 0 - 255)
       Byte 3 - Start time: seconds (binary 0 - 59)
       Byte 4 - Start time: frames (binary 0 - 74) */
    return (a * 60 * 75) + (b * 75) + c;
  }

  /* FAD mode
     Byte 2 - Start frame address (MSB)
     Byte 3 - Start frame address
     Byte 4 - Start frame address (LSB) */
  return (a << 16) | (b << 8) | c;
}

static void gdrom_spi_end(struct gdrom *gd) {
  gd->ireason.IO = 1;
  gd->ireason.CoD = 1;
  gd->status.DRDY = 1;
  gd->status.BSY = 0;
  gd->status.DRQ = 0;

  holly_raise_interrupt(gd->holly, HOLLY_INT_G1GDINT);

  gd->state = STATE_READ_ATA_CMD;
}

static void gdrom_spi_cdread(struct gdrom *gd) {
  if (gd->cdr_dma) {
    int max_dma_sectors = sizeof(gd->dma_buffer) / DISC_MAX_SECTOR_SIZE;

    /* fill DMA buffer with as many sectors as possible */
    int num_sectors = MIN(gd->cdr_num_sectors, max_dma_sectors);
    gd->dma_size = gdrom_read_sectors(gd, gd->cdr_first_sector, gd->cdr_secfmt,
                                      gd->cdr_secmask, num_sectors,
                                      gd->dma_buffer, sizeof(gd->dma_buffer));
    gd->dma_head = 0;

    /* update sector read state */
    gd->cdr_first_sector += num_sectors;
    gd->cdr_num_sectors -= num_sectors;

    /* gdrom state won't be updated until DMA transfer is completed */
    gd->state = STATE_WRITE_DMA_DATA;
  } else {
    int max_pio_sectors = sizeof(gd->pio_buffer) / DISC_MAX_SECTOR_SIZE;

    /* fill PIO buffer with as many sectors as possible */
    int num_sectors = MIN(gd->cdr_num_sectors, max_pio_sectors);
    gd->pio_size = gdrom_read_sectors(gd, gd->cdr_first_sector, gd->cdr_secfmt,
                                      gd->cdr_secmask, num_sectors,
                                      gd->pio_buffer, sizeof(gd->pio_buffer));
    gd->pio_head = 0;

    /* update sector read state */
    gd->cdr_first_sector += num_sectors;
    gd->cdr_num_sectors -= num_sectors;

    /* update gdrom state */
    gd->byte_count.full = gd->pio_size;
    gd->ireason.IO = 1;
    gd->ireason.CoD = 0;
    gd->status.DRQ = 1;
    gd->status.BSY = 0;

    holly_raise_interrupt(gd->holly, HOLLY_INT_G1GDINT);

    gd->state = STATE_WRITE_SPI_DATA;
  }
}

static void gdrom_spi_read(struct gdrom *gd, int offset, int size) {
  gd->cdr_num_sectors = 0;

  gd->pio_head = 0;
  gd->pio_size = size;
  gd->pio_offset = offset;

  gd->byte_count.full = size;
  gd->ireason.IO = 1;
  gd->ireason.CoD = 0;
  gd->status.DRQ = 1;
  gd->status.BSY = 0;

  holly_raise_interrupt(gd->holly, HOLLY_INT_G1GDINT);

  gd->state = STATE_READ_SPI_DATA;
}

static void gdrom_spi_write(struct gdrom *gd, void *data, int size) {
  gd->cdr_num_sectors = 0;

  CHECK(size < (int)sizeof(gd->pio_buffer));
  memcpy(gd->pio_buffer, data, size);
  gd->pio_size = size;
  gd->pio_head = 0;

  gd->byte_count.full = gd->pio_size;
  gd->ireason.IO = 1;
  gd->ireason.CoD = 0;
  gd->status.DRQ = 1;
  gd->status.BSY = 0;

  holly_raise_interrupt(gd->holly, HOLLY_INT_G1GDINT);

  gd->state = STATE_WRITE_SPI_DATA;
}

static void gdrom_ata_end(struct gdrom *gd) {
  gd->status.DRDY = 1;
  gd->status.BSY = 0;

  holly_raise_interrupt(gd->holly, HOLLY_INT_G1GDINT);

  gd->state = STATE_READ_ATA_CMD;
}

static void gdrom_spi_data(struct gdrom *gd, int arg) {
  /* only used by SET_MODE */
  int offset = gd->pio_offset;
  uint8_t *data = gd->pio_buffer;
  int size = gd->pio_size;
  memcpy((uint8_t *)&gd->hw_info + offset, data, size);

  gdrom_spi_end(gd);
}

static void gdrom_pio_read(struct gdrom *gd, int arg) {
  if (gd->pio_head == gd->pio_size) {
    if (gd->cdr_num_sectors) {
      gdrom_spi_cdread(gd);
    } else {
      gdrom_spi_end(gd);
    }
  }
}

static void gdrom_spi_cmd(struct gdrom *gd, int arg) {
  uint8_t *data = gd->pio_buffer;
  int cmd = data[0];

  LOG_GDROM("gdrom_spi_cmd 0x%x", cmd);

  gd->status.DRQ = 0;
  gd->status.BSY = 1;

  switch (cmd) {
    /*
     * packet command flow for pio data to host
     */
    case GD_SPI_REQ_STAT: {
      int offset = data[2];
      int size = data[4];

      struct gd_spi_status stat;
      gdrom_get_status(gd, &stat);

      /* bswap multibyte values to BE */
      stat.fad = bswap24(stat.fad);

      gdrom_spi_write(gd, (uint8_t *)&stat + offset, size);
    } break;

    case GD_SPI_REQ_MODE: {
      int offset = data[2];
      int size = data[4];

      gdrom_spi_write(gd, (uint8_t *)&gd->hw_info + offset, size);
    } break;

    case GD_SPI_REQ_ERROR: {
      int size = data[4];

      struct gd_spi_error err;
      gdrom_get_error(gd, &err);

      gdrom_spi_write(gd, &err, size);
    } break;

    case GD_SPI_GET_TOC: {
      int area = (data[1] & 0x1);
      int size = (data[3] << 8) | data[4];

      struct gd_spi_toc toc;
      gdrom_get_toc(gd, area, &toc);

      /* bswap multibyte values to BE */
      for (int i = 0; i < array_size(toc.entries); i++) {
        struct gd_spi_toc_entry *entry = &toc.entries[i];
        entry->fad = bswap24(entry->fad);
      }
      toc.leadout.fad = bswap24(toc.leadout.fad);

      gdrom_spi_write(gd, &toc, size);
    } break;

    case GD_SPI_REQ_SES: {
      int session = data[2];
      int size = data[4];

      struct gd_spi_session ses;
      gdrom_get_session(gd, session, &ses);

      /* bswap multibyte values to BE */
      ses.fad = bswap24(ses.fad);

      gdrom_spi_write(gd, &ses, sizeof(ses));
    } break;

    case GD_SPI_GET_SCD: {
      int format = data[1] & 0xf;
      int size = (data[3] << 8) | data[4];

      uint8_t scd[GD_SPI_SCD_SIZE];
      gdrom_get_subcode(gd, format, scd, sizeof(scd));

      gdrom_spi_write(gd, scd, size);
    } break;

    case GD_SPI_CD_READ: {
      int msf = (data[1] & 0x1);

      gd->cdr_dma = gd->features.dma;
      gd->cdr_secfmt = (data[1] & 0xe) >> 1;
      gd->cdr_secmask = (data[1] >> 4) & 0xff;
      gd->cdr_first_sector = gdrom_get_fad(data[2], data[3], data[4], msf);
      gd->cdr_num_sectors = (data[8] << 16) | (data[9] << 8) | data[10];

      gdrom_spi_cdread(gd);
    } break;

    case GD_SPI_CD_READ2: {
      LOG_FATAL("GD_SPI_CD_READ2");
    } break;

    /*
     * packet command flow for pio data from host
     */
    case GD_SPI_SET_MODE: {
      int offset = data[2];
      int size = data[4];

      gdrom_spi_read(gd, offset, size);
    } break;

    /*
     * non-data command flow
     */
    case GD_SPI_TEST_UNIT: {
      gdrom_spi_end(gd);
    } break;

    case GD_SPI_CD_OPEN: {
      LOG_FATAL("GD_SPI_CD_OPEN");
    } break;

    case GD_SPI_CD_PLAY: {
      LOG_WARNING("ignoring GD_SPI_CD_PLAY");

      gd->sectnum.status = GD_STATUS_PAUSE;

      gdrom_spi_end(gd);
    } break;

    case GD_SPI_CD_SEEK: {
      int param_type = data[1] & 0xf;

      LOG_WARNING("ignoring GD_SPI_CD_SEEK");

      switch (param_type) {
        case GD_SEEK_FAD:
        case GD_SEEK_MSF:
        case GD_SEEK_PAUSE:
          gd->sectnum.status = GD_STATUS_PAUSE;
          break;
        case GD_SEEK_STOP:
          gd->sectnum.status = GD_STATUS_STANDBY;
          break;
      }

      gdrom_spi_end(gd);
    } break;

    case GD_SPI_CD_SCAN: {
      LOG_WARNING("ignoring GD_SPI_CD_SCAN");

      gd->sectnum.status = GD_STATUS_PAUSE;

      gdrom_spi_end(gd);
    } break;

    /* GD_SPI_CHK_SECU and GD_SPI_REQ_SECU are part of an undocumented security
       check that has yet to be fully reverse engineered. the check doesn't seem
       to have any side effects, a canned response is sent when the results are
       requested */
    case GD_SPI_CHK_SECU: {
      gdrom_spi_end(gd);
    } break;

    case GD_SPI_REQ_SECU: {
      gdrom_spi_write(gd, (uint8_t *)reply_71, sizeof(reply_71));
    } break;

    default:
      LOG_FATAL("unsupported SPI command %d", cmd);
      break;
  }
}

static void gdrom_pio_write(struct gdrom *gd, int arg) {
  if (gd->state == STATE_READ_ATA_DATA && gd->pio_head == GD_SPI_CMD_SIZE) {
    gdrom_event(gd, EVENT_SPI_CMD, 0);
  } else if (gd->state == STATE_READ_SPI_DATA && gd->pio_head == gd->pio_size) {
    gdrom_event(gd, EVENT_SPI_DATA, 0);
  }
}

static void gdrom_ata_cmd(struct gdrom *gd, int cmd) {
  int read_data = 0;

  LOG_GDROM("gdrom_ata_cmd 0x%x", cmd);

  gd->status.DRDY = 0;
  gd->status.BSY = 1;

  /* error bits represent the status of the most recent command, clear before
    processing a new command */
  gd->error.full = 0;
  gd->status.CHECK = 0;

  switch (cmd) {
    case GD_ATA_NOP: {
      /* terminates the current command */
      gd->error.ABRT = 1;
      gd->status.CHECK = 1;
    } break;

    case GD_ATA_SOFT_RESET: {
      gdrom_set_disc(gd, gd->disc);
    } break;

    case GD_ATA_EXEC_DIAG: {
      LOG_FATAL("GD_ATA_EXEC_DIAG");
    } break;

    case GD_ATA_PACKET_CMD: {
      read_data = 1;
    } break;

    case GD_ATA_IDENTIFY_DEV: {
      LOG_FATAL("GD_ATA_IDENTIFY_DEV");
    } break;

    case GD_ATA_SET_FEATURES: {
      /* transfer mode settings are ignored */
    } break;

    default:
      LOG_FATAL("unsupported ATA command %d", cmd);
      break;
  }

  if (read_data) {
    gd->pio_head = 0;

    gd->ireason.CoD = 1;
    gd->ireason.IO = 0;
    gd->status.DRQ = 1;
    gd->status.BSY = 0;

    gd->state = STATE_READ_ATA_DATA;
  } else {
    gdrom_ata_end(gd);
  }
}

static void gdrom_event(struct gdrom *gd, enum gd_event ev, int arg) {
  gd_event_cb cb = gd_transitions[gd->state][ev];
  CHECK(cb);
  cb(gd, arg);
}

int gdrom_copy_sectors(struct gdrom *gd, int fad, int fmt, int mask,
                       int num_sectors, struct address_space *space,
                       uint32_t dst) {
  if (!gd->disc) {
    LOG_WARNING("gdrom_copy_sectors failed, no disc");
    return 0;
  }

  int read = 0;
  uint8_t tmp[DISC_MAX_SECTOR_SIZE];

  for (int i = fad; i < fad + num_sectors; i++) {
    int n = gdrom_read_sectors(gd, i, fmt, mask, 1, tmp, sizeof(tmp));
    as_memcpy_to_guest(space, dst + read, tmp, n);
    read += n;
  }

  return read;
}

int gdrom_read_sectors(struct gdrom *gd, int fad, int fmt, int mask,
                       int num_sectors, uint8_t *dst, int dst_size) {
  if (!gd->disc) {
    LOG_WARNING("gdrom_read_sectors failed, no disc");
    return 0;
  }

  int read = 0;
  uint8_t data[DISC_MAX_SECTOR_SIZE];

  LOG_GDROM("gdrom_read_sectors [%d, %d)", fad, fad + num_sectors);

  for (int i = fad; i < fad + num_sectors; i++) {
    int n = disc_read_sector(gd->disc, i, fmt, mask, data);
    CHECK_LE(read + n, dst_size);
    memcpy(dst + read, data, n);
    read += n;
  }

  return read;
}

void gdrom_get_subcode(struct gdrom *gd, int format, uint8_t *data, int size) {
  CHECK_NOTNULL(gd->disc);
  CHECK_GE(size, GD_SPI_SCD_SIZE);

  /* FIXME implement */
  memset(data, 0, GD_SPI_SCD_SIZE);
  data[1] = GD_AUDIO_NOSTATUS;

  switch (format) {
    case 0:
      data[2] = 0x0;
      data[3] = 0x64;
      break;
    case 1:
      data[2] = 0x0;
      data[3] = 0xe;
      break;
  }

  LOG_GDROM("gdrom_get_subcode not fully implemented");
}

void gdrom_get_session(struct gdrom *gd, int session_num,
                       struct gd_spi_session *ses) {
  CHECK_NOTNULL(gd->disc);

  memset(ses, 0, sizeof(*ses));

  ses->status = gd->sectnum.status;

  /* when session is 0 the "track_num" field contains the total number of
     sessions, while the "fad" field contains the lead-out fad

     when session is non-0, the "track_num" field contains the first track of
     the session, while the "fad" field contains contains the starting fad of
     the specified session */
  if (session_num == 0) {
    int num_sessions = disc_get_num_sessions(gd->disc);
    struct session *last_session = disc_get_session(gd->disc, num_sessions - 1);
    ses->track = num_sessions;
    ses->fad = last_session->leadout_fad;
  } else {
    struct session *session = disc_get_session(gd->disc, session_num - 1);
    struct track *first_track = disc_get_track(gd->disc, session->first_track);
    ses->track = first_track->num;
    ses->fad = first_track->fad;
  }
}

void gdrom_get_toc(struct gdrom *gd, int area, struct gd_spi_toc *toc) {
  CHECK_NOTNULL(gd->disc);

  struct track *first_track = NULL;
  struct track *last_track = NULL;
  int leadin_fad = 0;
  int leadout_fad = 0;
  disc_get_toc(gd->disc, area, &first_track, &last_track, &leadin_fad,
               &leadout_fad);

  /* 0xffffffff represents an invalid track */
  memset(toc, 0xff, sizeof(*toc));

  /* write out entries for each track */
  for (int i = first_track->num; i <= last_track->num; i++) {
    struct track *track = disc_get_track(gd->disc, i - 1);
    struct gd_spi_toc_entry *entry = &toc->entries[i - 1];

    entry->adr = track->adr;
    entry->ctrl = track->ctrl;
    entry->fad = track->fad;
  }

  toc->first.adr = first_track->adr;
  toc->first.ctrl = first_track->ctrl;
  toc->first.track_num = first_track->num;

  toc->last.adr = last_track->adr;
  toc->last.ctrl = last_track->ctrl;
  toc->last.track_num = last_track->num;

  toc->leadout.fad = leadout_fad;
}

void gdrom_get_error(struct gdrom *gd, struct gd_spi_error *err) {
  CHECK_NOTNULL(gd->disc);

  memset(err, 0, sizeof(*err));

  err->one = 0xf;

  /* TODO implement the sense key / code information */

  CHECK_EQ(sizeof(*err), 10);
}

void gdrom_get_status(struct gdrom *gd, struct gd_spi_status *stat) {
  CHECK_NOTNULL(gd->disc);

  memset(stat, 0, sizeof(*stat));

  stat->status = gd->sectnum.status;
  stat->repeat = 0;
  stat->format = gd->sectnum.format;
  stat->control = 0x4;
  stat->address = 0;
  stat->scd_track = 2;
  stat->scd_index = 0;
  stat->fad = 0x0;
}

static int gdrom_init(struct device *dev) {
  struct gdrom *gd = (struct gdrom *)dev;

  /* set default hardware information */
  memset(&gd->hw_info, 0, sizeof(gd->hw_info));
  gd->hw_info.speed = 0x0;
  gd->hw_info.standby_hi = 0x00;
  gd->hw_info.standby_lo = 0xb4;
  gd->hw_info.read_flags = 0x19;
  gd->hw_info.read_retry = 0x08;
  strncpy_pad_spaces(gd->hw_info.drive_info, "SE",
                     sizeof(gd->hw_info.drive_info));
  strncpy_pad_spaces(gd->hw_info.system_version, "Rev 6.43",
                     sizeof(gd->hw_info.system_version));
  strncpy_pad_spaces(gd->hw_info.system_date, "990408",
                     sizeof(gd->hw_info.system_date));

  gdrom_set_disc(gd, NULL);

  return 1;
}

void gdrom_set_drive_mode(struct gdrom *gd, struct gd_hw_info *info) {
  gd->hw_info = *info;
}

void gdrom_get_drive_mode(struct gdrom *gd, struct gd_hw_info *info) {
  *info = gd->hw_info;
}

void gdrom_dma_end(struct gdrom *gd) {
  LOG_GDROM("gd_dma_end");
}

int gdrom_dma_read(struct gdrom *gd, uint8_t *data, int n) {
  /* try to read more if the current dma buffer has been completely read */
  if (gd->dma_head >= gd->dma_size) {
    gdrom_spi_cdread(gd);
  }

  int remaining = gd->dma_size - gd->dma_head;
  n = MIN(n, remaining);
  CHECK_GT(n, 0);

  LOG_GDROM("gdrom_dma_read %d / %d bytes", gd->dma_head + n, gd->dma_size);
  memcpy(data, &gd->dma_buffer[gd->dma_head], n);
  gd->dma_head += n;

  if (gd->dma_head >= gd->dma_size) {
    LOG_GDROM("gdrom_dma cd_read complete");

    /* CD_READ command is now done */
    gdrom_spi_end(gd);
  }

  return n;
}

void gdrom_dma_begin(struct gdrom *gd) {
  CHECK(gd->dma_size);

  LOG_GDROM("gd_dma_begin");
}

void gdrom_set_disc(struct gdrom *gd, struct disc *disc) {
  if (gd->disc != disc) {
    if (gd->disc) {
      disc_destroy(gd->disc);
    }

    gd->disc = disc;

    /* print meta info */
    struct disc_meta meta;
    disc_get_meta(gd->disc, &meta);

    char name[256];
    char version[16];
    char id[16];
    strncpy_trim_spaces(name, meta.name, sizeof(meta.name));
    strncpy_trim_spaces(version, meta.version, sizeof(meta.version));
    strncpy_trim_spaces(id, meta.id, sizeof(meta.id));
    LOG_INFO("gdrom_set_disc %s %s - %s", name, version, id);
  }

  /* perform "soft reset" of internal state */
  gd->error.full = 0;

  gd->status.full = 0;
  gd->status.DRDY = 1;
  gd->status.BSY = 0;

  gd->sectnum.full = 0;
  if (gd->disc) {
    gd->sectnum.status = GD_STATUS_PAUSE;
    gd->sectnum.format = disc_get_format(disc);
  } else {
    gd->sectnum.status = GD_STATUS_NODISC;
  }

  /* TODO how do GD_FEATURES, GD_INTREASON, GD_BYCTLLO and GD_BYCTLHI behave */
}

void gdrom_destroy(struct gdrom *gd) {
  if (gd->disc) {
    disc_destroy(gd->disc);
  }

  dc_destroy_device((struct device *)gd);
}

struct gdrom *gdrom_create(struct dreamcast *dc) {
  struct gdrom *gd =
      dc_create_device(dc, sizeof(struct gdrom), "gdrom", &gdrom_init);
  return gd;
}

REG_R32(holly_cb, GD_ALTSTAT_DEVCTRL) {
  struct gdrom *gd = dc->gdrom;
  /* this register is the same as the status register, but it does not
     clear DMA status information when it is accessed */
  uint16_t value = gd->status.full;
  LOG_GDROM("read GD_ALTSTAT 0x%x", value);
  return value;
}

REG_W32(holly_cb, GD_ALTSTAT_DEVCTRL) {
  LOG_GDROM("write GD_DEVCTRL 0x%x [unimplemented]", value);
}

REG_R32(holly_cb, GD_DATA) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = *(uint16_t *)&gd->pio_buffer[gd->pio_head];

  LOG_GDROM("read GD_DATA 0x%x", value);

  gd->pio_head += 2;

  gdrom_event(gd, EVENT_PIO_READ, 0);

  return value;
}

REG_W32(holly_cb, GD_DATA) {
  struct gdrom *gd = dc->gdrom;

  LOG_GDROM("write GD_DATA 0x%x", value);

  *(uint16_t *)&gd->pio_buffer[gd->pio_head] = (uint16_t)(value & 0xffff);
  gd->pio_head += 2;

  gdrom_event(gd, EVENT_PIO_WRITE, 0);
}

REG_R32(holly_cb, GD_ERROR_FEATURES) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = gd->error.full;
  LOG_GDROM("read GD_ERROR 0x%x", value);
  return value;
}

REG_W32(holly_cb, GD_ERROR_FEATURES) {
  struct gdrom *gd = dc->gdrom;
  LOG_GDROM("write GD_FEATURES 0x%x", value);
  gd->features.full = value;
}

REG_R32(holly_cb, GD_INTREASON) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = gd->ireason.full;
  LOG_GDROM("read GD_INTREASON 0x%x", value);
  return value;
}

REG_W32(holly_cb, GD_INTREASON) {
  LOG_FATAL("invalid write to GD_INTREASON");
}

REG_R32(holly_cb, GD_SECTNUM) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = gd->sectnum.full;
  LOG_GDROM("read GD_SECTNUM 0x%x", value);
  return value;
}

REG_W32(holly_cb, GD_SECTNUM) {
  LOG_FATAL("invalid write to GD_SECTNUM");
}

REG_R32(holly_cb, GD_BYCTLLO) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = gd->byte_count.lo;
  LOG_GDROM("read GD_BYCTLLO 0x%x", value);
  return value;
}

REG_W32(holly_cb, GD_BYCTLLO) {
  struct gdrom *gd = dc->gdrom;
  LOG_GDROM("write GD_BYCTLLO 0x%x", value);
  gd->byte_count.lo = value;
}

REG_R32(holly_cb, GD_BYCTLHI) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = gd->byte_count.hi;
  LOG_GDROM("read GD_BYCTLHI 0x%x", value);
  return value;
}

REG_W32(holly_cb, GD_BYCTLHI) {
  struct gdrom *gd = dc->gdrom;
  LOG_GDROM("write GD_BYCTLHI 0x%x", value);
  gd->byte_count.hi = value;
}

REG_R32(holly_cb, GD_DRVSEL) {
  uint16_t value = 0;
  LOG_GDROM("read GD_DRVSEL 0x%x [unimplemented]", value);
  return value;
}

REG_W32(holly_cb, GD_DRVSEL) {
  LOG_GDROM("write GD_DRVSEL 0x%x [unimplemented]", value);
}

REG_R32(holly_cb, GD_STATUS_COMMAND) {
  struct gdrom *gd = dc->gdrom;
  uint16_t value = gd->status.full;
  LOG_GDROM("read GD_STATUS 0x%x", value);
  holly_clear_interrupt(gd->holly, HOLLY_INT_G1GDINT);
  return value;
}

REG_W32(holly_cb, GD_STATUS_COMMAND) {
  struct gdrom *gd = dc->gdrom;
  LOG_GDROM("write GD_COMMAND 0x%x", value);
  gdrom_event(gd, EVENT_ATA_CMD, value);
}
