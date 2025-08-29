/*
 * FloppyTool (ASL version) - v7
 * Layout:
 *   Row 1: Format | Copy | Verify | Quit
 *   Row 2: Read ADF | Write ADF | Verify ADF | About
 *
 * Changes in v7:
 *   - Removed the mid artwork area
 *   - Reduced window height
 *   - Added a 2–3 line ASCII banner between gadget rows and log
 *   - Progress bar kept compact and visible
 *
 * AmigaOS 2.0+ (v37), 68k
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <devices/trackdisk.h>

#include <intuition/intuition.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/dos.h>

#include <dos/dos.h>
#include <dos/dosextens.h>

#include <string.h>
#include <stdio.h>

#define APP_NAME "FloppyTool"
#define APP_VER  "v7"

/* ----- Base pointers ----- */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *AslBase       = NULL;
extern struct DosLibrary *DOSBase;

/* ----- Gadget IDs (main) ----- */
enum {
  GID_FORMAT=1, GID_COPY, GID_VERIFY, GID_QUIT,
  GID_READADF, GID_WRITEADF, GID_VERIFYADF, GID_ABOUT
};

/* ----- Window size & layout ----- */
#define WIN_W  500
#define WIN_H  248

/* Gadget row layout */
#define GAD_LEFT   12
#define GAD_W      110
#define GAD_H      18
#define GAD_GAP    12
#define ROW1_Y     20
#define ROW2_Y     (ROW1_Y + 22 + 10)

/* ASCII banner area between rows and log */
#define ASCII_Y    (ROW2_Y + GAD_H + 6)
#define ASCII_H    24  /* 2–3 text lines */

/* Log, progress, status */
#define LOG_Y       (ASCII_Y + ASCII_H + 6)
#define LOG_H       52  /* ~4 lines */
#define PROGRESS_Y  (WIN_H - 64)
#define STATUS_Y    (WIN_H - 24)

/* Floppy geometry (DD) */
#define CYLINDERS   80
#define HEADS       2
#define TRACKS      (CYLINDERS*HEADS)  /* 160 */
#define SECTORS     11
#define BYTES_PER_SECTOR 512
#define TRACK_SIZE  (SECTORS*BYTES_PER_SECTOR)  /* 5632 bytes */
#define DISK_SIZE   (TRACKS*TRACK_SIZE)         /* 901120 bytes */
#define TOTAL_SECTORS (TRACKS*SECTORS)          /* 1760 */

/* ----- UI state ----- */
struct AppUI {
  struct Window *win;
  struct Gadget *gadlist;
  APTR           vi;
  struct Gadget *gadFormat;
  struct Gadget *gadCopy;
  struct Gadget *gadVerify;
  struct Gadget *gadQuit;

  struct Gadget *gadReadADF;
  struct Gadget *gadWriteADF;
  struct Gadget *gadVerifyADF;
  struct Gadget *gadAbout;

  char logbuf[4][120];
  int  logcount;
} ui;

/* ----- Prototypes ----- */
static void CloseAll(void);
static BOOL OpenLibs(void);
static BOOL OpenUI(void);
static void CloseUI(void);

static void DrawStatus(const char *msg);
static void DrawProgress(ULONG done, ULONG total);
static void ClearProgress(void);
static void DrawFrame(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h);
static void DrawLog(void);
static void DrawAsciiBanner(void);
static void LogClear(void);
static void LogAdd(const char *msg);

static void DoFormatFloppy(void);
static void DoVerifyFloppy(void);
static void DoCopyFloppy(void);
static void DoReadADF(void);
static void DoWriteADF(void);
static void DoVerifyADF(void);
static void DoAbout(void);

static BOOL AskFloppyUnit(UBYTE *unitOut, CONST_STRPTR action);
static BOOL AskFormatQuick(BOOL *quickOut);
static BOOL AskVolumeName(char *outName, int maxlen, CONST_STRPTR defName);

/* ASL helpers (used by Write/Verify ADF) */
static BOOL PathSplit(CONST_STRPTR in, char *drawerOut, int dsz, char *fileOut, int fsz);
static BOOL ASL_OpenFile(char *outPath, int maxlen, CONST_STRPTR title, CONST_STRPTR defPath);

/* Raw/ADF ops */
static BOOL RawWritePass(UBYTE unit);
static BOOL RawVerify(UBYTE unit);
static BOOL RawCopyTwoDrives(UBYTE srcUnit, UBYTE dstUnit);
static BOOL RawCopyOneDrive(UBYTE unit);
static BOOL ADF_ReadFromDrive(UBYTE unit, CONST_STRPTR path);
static BOOL ADF_WriteToDrive(UBYTE unit, CONST_STRPTR path);
static BOOL OpenTD(UBYTE unit, struct MsgPort **pp, struct IOExtTD **pio);
static void CloseTD(struct MsgPort *p, struct IOExtTD *io);
static void SetFloppyMotor(UBYTE unit, BOOL on);

/* helpers */
static BOOL HasFile(CONST_STRPTR path);
static BOOL GenUniqueAdfPath(UBYTE unit, char *out, int maxlen);

/* CRC32 */
static ULONG crc32_init(void);
static ULONG crc32_update(ULONG crc, const UBYTE *buf, ULONG len);
static ULONG crc32_final(ULONG crc);

/* ========================= MAIN ========================= */

int main(void) {
  if (!OpenLibs()) return 20;
  if (!OpenUI())   { CloseAll(); return 10; }

  DrawStatus("Ready.");
  ClearProgress();
  LogClear();
  DrawAsciiBanner();

  BOOL running = TRUE;
  ULONG sigmask = 1UL << ui.win->UserPort->mp_SigBit;

  while (running) {
    ULONG sigs = Wait(sigmask);
    if (sigs & sigmask) {
      struct IntuiMessage *imsg;
      while ((imsg = GT_GetIMsg(ui.win->UserPort)) != NULL) {
        ULONG cls = imsg->Class;
        UWORD code = imsg->Code;
        APTR  iad = imsg->IAddress;
        GT_ReplyIMsg(imsg);

        switch (cls) {
          case IDCMP_GADGETUP: {
            struct Gadget *gad = (struct Gadget *)iad;
            switch (gad->GadgetID) {
              case GID_FORMAT:    DoFormatFloppy(); break;
              case GID_COPY:      DoCopyFloppy();   break;
              case GID_VERIFY:    DoVerifyFloppy(); break;
              case GID_QUIT:      running = FALSE;  break;

              case GID_READADF:   DoReadADF();      break;
              case GID_WRITEADF:  DoWriteADF();     break;
              case GID_VERIFYADF: DoVerifyADF();    break;
              case GID_ABOUT:     DoAbout();        break;
            }
          } break;

          case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(ui.win);
            DrawAsciiBanner();
            DrawLog();
            GT_EndRefresh(ui.win, TRUE);
            break;

          case IDCMP_VANILLAKEY:
            if (code == 27) running = FALSE;
            break;

          case IDCMP_CLOSEWINDOW:
            running = FALSE;
            break;
        }
      }
    }
  }

  CloseAll();
  return 0;
}

/* ========================= IMPLEMENTAZIONE ========================= */

static BOOL OpenLibs(void) {
  IntuitionBase = (struct IntuitionBase*)OpenLibrary("intuition.library", 37);
  if (!IntuitionBase) return FALSE;

  GfxBase = (struct GfxBase*)OpenLibrary("graphics.library", 37);
  if (!GfxBase) return FALSE;

  GadToolsBase = OpenLibrary("gadtools.library", 37);
  if (!GadToolsBase) return FALSE;

  AslBase = OpenLibrary("asl.library", 37);
  if (!AslBase) return FALSE;

  DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 37);
  if (!DOSBase) return FALSE;

  return TRUE;
}

static BOOL OpenUI(void) {
  ui.win = OpenWindowTags(NULL,
    WA_Left,        90,
    WA_Top,         60,
    WA_Width,       WIN_W,
    WA_Height,      WIN_H,
    WA_Title,       (ULONG)APP_NAME " " APP_VER,
    WA_DragBar,     TRUE,
    WA_DepthGadget, TRUE,
    WA_CloseGadget, TRUE,
    WA_Activate,    TRUE,
    WA_SimpleRefresh, TRUE,
    WA_GimmeZeroZero, TRUE,
    WA_IDCMP,       IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
    TAG_END
  );
  if (!ui.win) return FALSE;

  ui.vi = GetVisualInfo(ui.win->WScreen, TAG_END);
  if (!ui.vi) return FALSE;

  CreateContext(&ui.gadlist);

  struct NewGadget ng;
  memset(&ng, 0, sizeof(ng));
  ng.ng_VisualInfo = ui.vi;
  ng.ng_TextAttr   = NULL;

  const UWORD left = GAD_LEFT;
  const UWORD top1 = ROW1_Y;
  const UWORD top2 = ROW2_Y;
  const UWORD w    = GAD_W;
  const UWORD h    = GAD_H;
  const UWORD gap  = GAD_GAP;

  /* Row 1: Format | Copy | Verify | Quit */
  ng.ng_LeftEdge  = left;
  ng.ng_TopEdge   = top1;
  ng.ng_Width     = w;
  ng.ng_Height    = h;
  ng.ng_GadgetText= (UBYTE*)"Format (DF*)";
  ng.ng_GadgetID  = GID_FORMAT;
  ui.gadFormat = CreateGadget(BUTTON_KIND, ui.gadlist, &ng, TAG_END);
  if (!ui.gadFormat) return FALSE;

  ng.ng_LeftEdge  = left + (w+gap);
  ng.ng_GadgetText= (UBYTE*)"Copy (raw)";
  ng.ng_GadgetID  = GID_COPY;
  ui.gadCopy = CreateGadget(BUTTON_KIND, ui.gadFormat, &ng, TAG_END);
  if (!ui.gadCopy) return FALSE;

  ng.ng_LeftEdge  = left + 2*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"Verify";
  ng.ng_GadgetID  = GID_VERIFY;
  ui.gadVerify = CreateGadget(BUTTON_KIND, ui.gadCopy, &ng, TAG_END);
  if (!ui.gadVerify) return FALSE;

  ng.ng_LeftEdge  = left + 3*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"Quit";
  ng.ng_GadgetID  = GID_QUIT;
  ui.gadQuit = CreateGadget(BUTTON_KIND, ui.gadVerify, &ng, TAG_END);
  if (!ui.gadQuit) return FALSE;

  /* Row 2: Read ADF | Write ADF | Verify ADF | About */
  ng.ng_LeftEdge  = left;
  ng.ng_TopEdge   = top2;
  ng.ng_GadgetText= (UBYTE*)"Read ADF";
  ng.ng_GadgetID  = GID_READADF;
  ui.gadReadADF = CreateGadget(BUTTON_KIND, ui.gadQuit, &ng, TAG_END);
  if (!ui.gadReadADF) return FALSE;

  ng.ng_LeftEdge  = left + (w+gap);
  ng.ng_GadgetText= (UBYTE*)"Write ADF";
  ng.ng_GadgetID  = GID_WRITEADF;
  ui.gadWriteADF = CreateGadget(BUTTON_KIND, ui.gadReadADF, &ng, TAG_END);
  if (!ui.gadWriteADF) return FALSE;

  ng.ng_LeftEdge  = left + 2*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"Verify ADF";
  ng.ng_GadgetID  = GID_VERIFYADF;
  ui.gadVerifyADF = CreateGadget(BUTTON_KIND, ui.gadWriteADF, &ng, TAG_END);
  if (!ui.gadVerifyADF) return FALSE;

  ng.ng_LeftEdge  = left + 3*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"About";
  ng.ng_GadgetID  = GID_ABOUT;
  ui.gadAbout = CreateGadget(BUTTON_KIND, ui.gadVerifyADF, &ng, TAG_END);
  if (!ui.gadAbout) return FALSE;

  AddGList(ui.win, ui.gadlist, (UWORD)-1, (UWORD)-1, NULL);
  RefreshGList(ui.gadlist, ui.win, NULL, (UWORD)-1);
  GT_BeginRefresh(ui.win);
  GT_EndRefresh(ui.win, TRUE);
  GT_RefreshWindow(ui.win, NULL);
  DrawAsciiBanner();

  return TRUE;
}

static void DrawStatus(const char *msg) {
  if (!ui.win || !msg) return;
  struct RastPort *rp = ui.win->RPort;
  const WORD x = 12;
  const WORD y = STATUS_Y;
  const WORD w = WIN_W - 24;
  const WORD h = 16;
  SetAPen(rp, 0);
  RectFill(rp, x-2, y-12, x-2 + w, y-12 + h + 6);
  SetAPen(rp, 1);
  Move(rp, x, y);
  Text(rp, (STRPTR)msg, (ULONG)strlen(msg));
}

static void DrawFrame(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h) {
  if (!rp) return;
  Move(rp, x,     y);     Draw(rp, x+w, y);
  Move(rp, x+w,   y);     Draw(rp, x+w, y+h);
  Move(rp, x+w,   y+h);   Draw(rp, x,   y+h);
  Move(rp, x,     y+h);   Draw(rp, x,   y);
}

static void DrawProgress(ULONG done, ULONG total) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, y = PROGRESS_Y, w = WIN_W - 24, h = 12;
  SetAPen(rp, 1);
  DrawFrame(rp, x, y, w, h);
  ULONG frac = (total>0) ? (done * w) / total : 0;
  if (frac > (ULONG)w) frac = w;
  SetAPen(rp, 2);
  RectFill(rp, x+1, y+1, x+(WORD)frac, y+h-1);
}

static void ClearProgress(void) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, y = PROGRESS_Y, w = WIN_W - 24, h = 12;
  SetAPen(rp, 0);
  RectFill(rp, x-2, y-2, x+w+2, y+h+2);
  SetAPen(rp, 1);
  DrawFrame(rp, x, y, w, h);
}

static void DrawLog(void) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, y = LOG_Y, w = WIN_W - 24, h = LOG_H;
  SetAPen(rp, 0);
  RectFill(rp, x, y, x+w, y+h);
  SetAPen(rp, 1);
  for (int i=0;i<ui.logcount;i++) {
    Move(rp, x+4, y+14 + 12*i);
    Text(rp, (STRPTR)ui.logbuf[i], (ULONG)strlen(ui.logbuf[i]));
  }
  DrawFrame(rp, x, y, w, h);
}

/* ====== ASCII banner (2–3 text lines across width) ====== */
static void DrawAsciiBanner(void) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, w = WIN_W - 24;
  WORD y0 = ASCII_Y;
  WORD y1 = ASCII_Y + 12;
  WORD y2 = ASCII_Y + 22;

  /* clear area */
  SetAPen(rp, 0);
  RectFill(rp, x, ASCII_Y, x+w, ASCII_Y + ASCII_H);

  /* helper: repeat a small pattern across width */
  const char *p1 = "=-";
  const char *p2 = "-=";
  ULONG p1w = TextLength(rp, (STRPTR)p1, (ULONG)strlen(p1));
  ULONG p2w = TextLength(rp, (STRPTR)p2, (ULONG)strlen(p2));

  /* line 1 */
  SetAPen(rp, 2);
  WORD cx = x;
  while (cx < x + w) {
    Move(rp, cx, y0);
    Text(rp, (STRPTR)p1, (ULONG)strlen(p1));
    cx += (WORD)p1w;
  }

  /* centered title line */
  const char *title = "2025 - Danilo Savioni + Stella AI";
  ULONG tlw = TextLength(rp, (STRPTR)title, (ULONG)strlen(title));
  WORD tx = x + (w - (WORD)tlw) / 2;
  if (tx < x) tx = x;
  SetDrMd(rp, JAM2);           /* draw fg+bg for readability */
  SetAPen(rp, 0);              /* black text */
  SetBPen(rp, 2);              /* light background under glyphs */
  Move(rp, tx,   y1);
  Text(rp, (STRPTR)title, (ULONG)strlen(title));
  SetDrMd(rp, JAM1);

  /* line 3 */
  SetAPen(rp, 2);
  cx = x;
  while (cx < x + w) {
    Move(rp, cx, y2);
    Text(rp, (STRPTR)p2, (ULONG)strlen(p2));
    cx += (WORD)p2w;
  }
}

static void LogClear(void) {
  ui.logcount = 0;
  for (int i=0;i<4;i++) ui.logbuf[i][0] = '\0';
  DrawLog();
}

static void LogAdd(const char *msg) {
  if (!msg) return;
  if (ui.logcount < 4) {
    strncpy(ui.logbuf[ui.logcount], msg, sizeof(ui.logbuf[ui.logcount])-1);
    ui.logbuf[ui.logcount][sizeof(ui.logbuf[ui.logcount])-1] = '\0';
    ui.logcount++;
  } else {
    for (int i=0;i<3;i++) {
      strncpy(ui.logbuf[i], ui.logbuf[i+1], sizeof(ui.logbuf[i])-1);
      ui.logbuf[i][sizeof(ui.logbuf[i])-1] = '\0';
    }
    strncpy(ui.logbuf[3], msg, sizeof(ui.logbuf[3])-1);
    ui.logbuf[3][sizeof(ui.logbuf[3])-1] = '\0';
  }
  DrawLog();
}

/* ====== Simple dialogs ====== */

static BOOL AskFloppyUnit(UBYTE *unitOut, CONST_STRPTR action) {
  if (!unitOut) return FALSE;
  *unitOut = 0xFF;

  static UBYTE title[] = APP_NAME " " APP_VER;
  UBYTE body[96];
  sprintf((char*)body, "Select FLOPPY drive for %s", action ? action : "operation");

  struct EasyStruct es;
  es.es_StructSize = sizeof(struct EasyStruct);
  es.es_Flags      = 0;
  es.es_Title      = title;
  es.es_TextFormat = body;
  es.es_GadgetFormat = (UBYTE*)"DF0|DF1|DF2|DF3|Cancel";

  LONG sel = EasyRequestArgs(ui.win, &es, NULL, NULL);
  if (sel == 0 || sel == 5) return FALSE;
  if (sel < 1 || sel > 4) return FALSE;
  *unitOut = (UBYTE)(sel - 1);
  return TRUE;
}

static BOOL AskFormatQuick(BOOL *quickOut) {
  if (!quickOut) return FALSE;
  *quickOut = TRUE;
  static UBYTE title[] = APP_NAME " " APP_VER;
  static UBYTE text[]  = "Choose format type";
  static UBYTE gadgets[] = "Quick|Full|Cancel";
  struct EasyStruct es = { sizeof(struct EasyStruct), 0, title, text, gadgets };
  LONG sel = EasyRequestArgs(ui.win, &es, NULL, NULL);
  if (sel == 0 || sel == 3) return FALSE;
  *quickOut = (sel == 1) ? TRUE : FALSE;
  return TRUE;
}

static BOOL AskVolumeName(char *outName, int maxlen, CONST_STRPTR defName) {
  if (!outName || maxlen < 2) return FALSE;
  strncpy(outName, defName ? defName : "Untitled", maxlen-1);
  outName[maxlen-1] = '\0';
  return TRUE;
}

/* ========= ASL FILE REQUESTERS (for WRITE/VERIFY ADF) ========= */

static BOOL PathSplit(CONST_STRPTR in, char *drawerOut, int dsz, char *fileOut, int fsz) {
  if (!in || !drawerOut || !fileOut) return FALSE;
  drawerOut[0] = 0; fileOut[0] = 0;
  const char *lastSlash = NULL;
  const char *p = in;
  while (*p) { if (*p == '/' || *p == '\\') lastSlash = p; p++; }
  const char *colon = strchr(in, ':');
  if (lastSlash) {
    int dlen = (int)(lastSlash - in + 1);
    if (dlen >= dsz) dlen = dsz-1;
    strncpy(drawerOut, in, dlen); drawerOut[dlen] = '\0';
    strncpy(fileOut, lastSlash+1, fsz-1); fileOut[fsz-1] = '\0';
  } else {
    if (colon) {
      int vlen = (int)(colon - in + 1);
      if (vlen >= dsz) vlen = dsz-1;
      strncpy(drawerOut, in, vlen); drawerOut[vlen] = '\0';
      strncpy(fileOut, colon+1, fsz-1); fileOut[fsz-1] = '\0';
    } else {
      strncpy(drawerOut, "", dsz);
      strncpy(fileOut, in, fsz-1); fileOut[fsz-1] = '\0';
    }
  }
  return TRUE;
}

static BOOL ASL_OpenFile(char *outPath, int maxlen, CONST_STRPTR title, CONST_STRPTR defPath) {
  if (!outPath || maxlen < 4) return FALSE;
  outPath[0] = '\0';
  char drawer[256] = "RAM:";
  char file[108]   = "floppy.adf";
  if (defPath) PathSplit(defPath, drawer, sizeof(drawer), file, sizeof(file));

  struct FileRequester *fr = (struct FileRequester*)AllocAslRequestTags(ASL_FileRequest,
    ASLFR_TitleText,    (ULONG)(title ? title : "Open ADF"),
    ASLFR_InitialDrawer,(ULONG)drawer,
    ASLFR_InitialFile,  (ULONG)file,
    TAG_END);
  if (!fr) return FALSE;

  BOOL ok = FALSE;
  if (AslRequest(fr, NULL)) {
    const char *d = fr->fr_Drawer ? fr->fr_Drawer : "";
    const char *f = fr->fr_File   ? fr->fr_File   : "";
    if (d[0]) {
      int needsSlash = 0;
      int dl = strlen(d);
      if (dl>0) {
        char last = d[dl-1];
        needsSlash = !(last == ':' || last == '/' || last == '\\');
      }
      if (needsSlash)
        snprintf(outPath, maxlen, "%s/%s", d, f);
      else
        snprintf(outPath, maxlen, "%s%s", d, f);
    } else {
      snprintf(outPath, maxlen, "%s", f);
    }
    ok = (outPath[0] != '\0');
  }

  FreeAslRequest(fr);
  return ok;
}

/* ========================= ACTIONS ========================= */

static void DoFormatFloppy(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "FORMAT")) { DrawStatus("Format canceled."); return; }
  BOOL quick = TRUE;
  if (!AskFormatQuick(&quick)) { DrawStatus("Format canceled."); return; }

  char volname[32]; if (!AskVolumeName(volname, sizeof(volname), "Untitled")) { DrawStatus("Format canceled."); return; }

  const char *candidates[] = { "C:Format", "SYS:C/Format", NULL };
  const char *fmt = NULL;
  for (int i=0; candidates[i]; ++i) { if (HasFile(candidates[i])) { fmt = candidates[i]; break; } }
  if (!fmt) { DrawStatus("No Format in C: (install Workbench C:Format)."); return; }

  {
    char cmd[256];
    sprintf(cmd, "%s DRIVE DF%u: NAME \"%s\" QUICK", fmt, (unsigned)unit, volname);

    BPTR inTmp = Open("T:ft_yes", MODE_NEWFILE);
    if (inTmp) { Write(inTmp, (APTR)"y\n", 2); Close(inTmp); }
    BPTR in  = Open("T:ft_yes", MODE_OLDFILE);
    BPTR out = Open("NIL:", MODE_NEWFILE);

    LogClear(); LogAdd("Running C:Format QUICK");
    DrawStatus("Quick format...");
    LONG ok = Execute((STRPTR)cmd, in ? in : Open("NIL:", MODE_OLDFILE), out);
    if (in) Close(in);
    if (out) Close(out);
    DeleteFile("T:ft_yes");

    if (!ok) { SetFloppyMotor(unit, FALSE); DrawStatus("Format failed."); ClearProgress(); return; }
  }

  if (!quick) {
    DrawStatus("Full format: writing tracks...");
    ClearProgress();
    LogClear();
    BOOL passOk = RawWritePass(unit);
    SetFloppyMotor(unit, FALSE);
    DrawStatus(passOk ? "Full format done." : "Full format failed.");
    ClearProgress();
  } else {
    SetFloppyMotor(unit, FALSE);
    DrawStatus("Quick format done.");
    ClearProgress();
  }
}

static void DoVerifyFloppy(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "VERIFY")) { DrawStatus("Verify canceled."); return; }
  LogClear();
  DrawStatus("Verify: reading tracks...");
  BOOL ok = RawVerify(unit);
  SetFloppyMotor(unit, FALSE);
  DrawStatus(ok ? "Verify OK." : "Verify FAILED (read error).");
  ClearProgress();
}

static void DoCopyFloppy(void) {
  UBYTE src, dst;
  if (!AskFloppyUnit(&src, "COPY (source)")) { DrawStatus("Copy canceled."); return; }
  if (!AskFloppyUnit(&dst, "COPY (destination)")) { DrawStatus("Copy canceled."); return; }

  LogClear();
  DrawStatus("Copy (raw) in progress...");
  DrawProgress(0, DISK_SIZE);

  BOOL ok;
  if (src == dst) ok = RawCopyOneDrive(src);
  else            ok = RawCopyTwoDrives(src, dst);

  SetFloppyMotor(src, FALSE);
  SetFloppyMotor(dst, FALSE);

  DrawStatus(ok ? "Copy completed." : "Copy failed.");
  ClearProgress();
}

/* Read ADF: select only DFx, auto path RAM:DF<unit>_<n>.adf */
static void DoReadADF(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "READ ADF (source DFx:)")) { DrawStatus("Read ADF canceled."); return; }

  char path[300];
  if (!GenUniqueAdfPath(unit, path, sizeof(path))) { DrawStatus("Cannot build RAM: output path"); return; }

  char msg[160]; sprintf(msg, "Saving to %s", path); LogClear(); LogAdd(msg);
  DrawStatus("Reading DFx: to ADF...");
  BOOL ok = ADF_ReadFromDrive(unit, path);
  SetFloppyMotor(unit, FALSE);
  DrawStatus(ok ? "ADF saved." : "ADF read failed.");
  ClearProgress();
}

static void DoWriteADF(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "WRITE ADF (destination DFx:)")) { DrawStatus("Write ADF canceled."); return; }

  char path[300];
  if (!ASL_OpenFile(path, sizeof(path), "Select ADF to write...", "RAM:floppy.adf")) { DrawStatus("Write ADF canceled."); return; }

  LogClear();
  DrawStatus("Writing ADF to DFx: ...");
  BOOL ok = ADF_WriteToDrive(unit, path);
  SetFloppyMotor(unit, FALSE);
  DrawStatus(ok ? "ADF written to disk." : "ADF write failed.");
  ClearProgress();
}

static void DoVerifyADF(void) {
  char path[300];
  if (!ASL_OpenFile(path, sizeof(path), "Select ADF to verify...", "RAM:floppy.adf")) { DrawStatus("Verify ADF canceled."); return; }

  LogClear();
  DrawStatus("Verifying ADF...");
  BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
  if (!fh) { LogAdd("Cannot open ADF"); DrawStatus("Verify ADF failed."); return; }

  /* Size via ExamineFH (fallback Seek) */
  LONG size = -1;
  struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
  if (fib && ExamineFH(fh, fib)) size = fib->fib_Size;
  else {
    LONG end = Seek(fh, 0, OFFSET_END); if (end >= 0) size = end;
    Seek(fh, 0, OFFSET_BEGINNING);
  }
  if (fib) FreeVec(fib);
  char smsg[120]; sprintf(smsg, "ADF size: %ld bytes", (long)size); LogAdd(smsg);

  if (size != (LONG)DISK_SIZE) {
    LogAdd("Warning: size is not 901,120 bytes");
  }
  Seek(fh, 0, OFFSET_BEGINNING);

  /* CRC32 */
  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { Close(fh); LogAdd("No memory for CRC"); DrawStatus("Verify ADF failed."); return; }

  ULONG crc = crc32_init();
  LONG remaining = size > 0 ? size : 0;
  while (remaining > 0) {
    LONG chunk = (remaining >= TRACK_SIZE) ? TRACK_SIZE : remaining;
    LONG rd = Read(fh, buf, chunk);
    if (rd != chunk) { LogAdd("File read error during CRC"); FreeVec(buf); Close(fh); DrawStatus("Verify ADF failed."); return; }
    crc = crc32_update(crc, buf, (ULONG)chunk);
    remaining -= chunk;
    DrawProgress(size - remaining, size > 0 ? (ULONG)size : 1);
  }
  crc = crc32_final(crc);

  FreeVec(buf);
  Close(fh);

  char cmsg[120]; sprintf(cmsg, "CRC32: %08lx", (ULONG)crc);
  LogAdd(cmsg);
  DrawStatus((size == (LONG)DISK_SIZE) ? "ADF looks OK (size+CRC computed)." : "ADF verified (non-standard size).");
  ClearProgress();
}

static void DoAbout(void) {
  static UBYTE title[] = APP_NAME " " APP_VER;
  static UBYTE text[]  =
    "FloppyTool  Amiga DD floppy helper\n"
    "\n"
    "Features:\n"
    "    Format/Verify/Copy raw\n"
    "    Read/Write/Verify ADF\n"
    "\n"
    "  2025 Danilo Savioni + Stella\n"
    "Built with love for AmigaOS 2.0+ (68k)\n";
  static UBYTE gadgets[] = "OK";
  struct EasyStruct es = { sizeof(struct EasyStruct), 0, title, text, gadgets };
  EasyRequestArgs(ui.win, &es, NULL, NULL);
}

/* ====== Raw ops via trackdisk.device ====== */

static BOOL OpenTD(UBYTE unit, struct MsgPort **pp, struct IOExtTD **pio) {
  if (!pp || !pio) return FALSE;
  *pp = NULL; *pio = NULL;

  struct MsgPort *port = CreateMsgPort();
  if (!port) return FALSE;
  struct IOExtTD *io = (struct IOExtTD*)CreateIORequest(port, sizeof(struct IOExtTD));
  if (!io) { DeleteMsgPort(port); return FALSE; }

  if (OpenDevice("trackdisk.device", unit, (struct IORequest*)io, 0) != 0) {
    DeleteIORequest((struct IORequest*)io);
    DeleteMsgPort(port);
    return FALSE;
  }
  *pp = port; *pio = io;
  return TRUE;
}

static void CloseTD(struct MsgPort *p, struct IOExtTD *io) {
  if (io) { CloseDevice((struct IORequest*)io); DeleteIORequest((struct IORequest*)io); }
  if (p)  { DeleteMsgPort(p); }
}

static void SetFloppyMotor(UBYTE unit, BOOL on) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return;
  io->iotd_Req.io_Command = TD_MOTOR;
  io->iotd_Req.io_Length  = on ? 1 : 0;
  (void)DoIO((struct IORequest*)io);
  CloseTD(p, io);
}

static BOOL RawWritePass(UBYTE unit) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return FALSE;

  SetFloppyMotor(unit, TRUE);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { CloseTD(p, io); return FALSE; }
  memset(buf, 0, TRACK_SIZE);

  BOOL ok = TRUE;
  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_WRITE;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) {
      ok = FALSE;
      char m[80]; sprintf(m, "Write error at track %lu (io_Error=%ld)", (unsigned long)t, (long)io->iotd_Req.io_Error);
      LogAdd(m);
      break;
    }
    DrawProgress(t+1, TRACKS);
    if ((t % 4) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Wrote track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  CloseTD(p, io);
  return ok;
}

static BOOL RawVerify(UBYTE unit) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return FALSE;

  SetFloppyMotor(unit, TRUE);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { CloseTD(p, io); return FALSE; }

  ULONG doneSectors = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    LONG err = DoIO((struct IORequest*)io);
    if (err != 0) {
      ok = FALSE;
      char m[80]; sprintf(m, "Read error at track %lu (io_Error=%ld)", (unsigned long)t, (long)io->iotd_Req.io_Error);
      LogAdd(m);
      break;
    }
    doneSectors += SECTORS;
    DrawProgress(doneSectors, TOTAL_SECTORS);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[80]; sprintf(m, "Track %lu/%u, sectors %lu/%u", (unsigned long)(t+1), TRACKS, (unsigned long)doneSectors, (unsigned)TOTAL_SECTORS); LogAdd(m); }
  }

  FreeVec(buf);
  CloseTD(p, io);
  return ok;
}

static BOOL RawCopyTwoDrives(UBYTE srcUnit, UBYTE dstUnit) {
  struct MsgPort *ps = NULL; struct IOExtTD *is = NULL;
  struct MsgPort *pd = NULL; struct IOExtTD *id = NULL;

  if (!OpenTD(srcUnit, &ps, &is)) return FALSE;
  if (!OpenTD(dstUnit, &pd, &id)) { CloseTD(ps, is); return FALSE; }

  SetFloppyMotor(srcUnit, TRUE);
  SetFloppyMotor(dstUnit, TRUE);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { CloseTD(ps, is); CloseTD(pd, id); return FALSE; }

  ULONG done = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    is->iotd_Req.io_Command = CMD_READ;
    is->iotd_Req.io_Data    = (APTR)buf;
    is->iotd_Req.io_Length  = TRACK_SIZE;
    is->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)is) != 0) { ok = FALSE; LogAdd("Read error"); break; }

    id->iotd_Req.io_Command = CMD_WRITE;
    id->iotd_Req.io_Data    = (APTR)buf;
    id->iotd_Req.io_Length  = TRACK_SIZE;
    id->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)id) != 0) { ok = FALSE; LogAdd("Write error"); break; }

    done += TRACK_SIZE;
    DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  CloseTD(ps, is);
  CloseTD(pd, id);
  return ok;
}

static BOOL RawCopyOneDrive(UBYTE unit) {
  BOOL ok = FALSE;
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return FALSE;

  SetFloppyMotor(unit, TRUE);

  UBYTE *image = (UBYTE*)AllocVec(DISK_SIZE, MEMF_CLEAR);
  if (!image) { CloseTD(p, io); return FALSE; }

  ULONG done = 0;
  DrawStatus("Reading source to RAM (swap later)...");
  ClearProgress();
  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data    = (APTR)(image + t*TRACK_SIZE);
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { LogAdd("Read error"); goto cleanup; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  ClearProgress();
  done = 0;

  struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE*)APP_NAME,
                           (UBYTE*)"Insert DESTINATION disk and click Continue",
                           (UBYTE*)"Continue|Cancel" };
  LONG sel = EasyRequestArgs(ui.win, &es, NULL, NULL);
  if (sel != 1) goto cleanup;

  DrawStatus("Writing RAM image to destination...");
  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_WRITE;
    io->iotd_Req.io_Data    = (APTR)(image + t*TRACK_SIZE);
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { LogAdd("Write error"); goto cleanup; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  ok = TRUE;

cleanup:
  FreeVec(image);
  CloseTD(p, io);
  return ok;
}

/* ====== ADF I/O ====== */

static BOOL ADF_ReadFromDrive(UBYTE unit, CONST_STRPTR path) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) { LogAdd("Open trackdisk failed"); return FALSE; }

  SetFloppyMotor(unit, TRUE);

  BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
  if (!fh) { CloseTD(p, io); LogAdd("Cannot create ADF file"); return FALSE; }

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { Close(fh); CloseTD(p, io); LogAdd("No memory"); return FALSE; }

  ULONG done = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { ok = FALSE; LogAdd("Read error"); break; }
    LONG wr = Write(fh, buf, TRACK_SIZE);
    if (wr != TRACK_SIZE) { ok = FALSE; LogAdd("File write error"); break; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  Close(fh);
  CloseTD(p, io);

  if (ok) {
    BPTR fh2 = Open((STRPTR)path, MODE_OLDFILE);
    if (fh2) {
      struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
      if (fib && ExamineFH(fh2, fib)) {
        char m[100]; sprintf(m, "Saved ADF size: %ld bytes", (long)fib->fib_Size);
        LogAdd(m);
      }
      if (fib) FreeVec(fib);
      Close(fh2);
    }
  }

  return ok;
}

static BOOL ADF_WriteToDrive(UBYTE unit, CONST_STRPTR path) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) { LogAdd("Open trackdisk failed"); return FALSE; }

  SetFloppyMotor(unit, TRUE);

  BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
  if (!fh) { CloseTD(p, io); LogAdd("Cannot open ADF"); return FALSE; }

  LONG size = -1;
  struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
  if (fib && ExamineFH(fh, fib)) {
    size = fib->fib_Size;
  } else {
    LONG end = Seek(fh, 0, OFFSET_END);
    if (end >= 0) size = end;
    Seek(fh, 0, OFFSET_BEGINNING);
  }
  if (fib) FreeVec(fib);

  char smsg[96]; sprintf(smsg, "Detected ADF size: %ld bytes", (long)size); LogAdd(smsg);

  if (size != (LONG)DISK_SIZE) {
    Close(fh); CloseTD(p, io);
    LogAdd("Invalid ADF size (need 901,120 bytes)");
    return FALSE;
  }
  Seek(fh, 0, OFFSET_BEGINNING);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { Close(fh); CloseTD(p, io); LogAdd("No memory"); return FALSE; }

  ULONG done = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    LONG rd = Read(fh, buf, TRACK_SIZE);
    if (rd != TRACK_SIZE) { ok = FALSE; LogAdd("File read error"); break; }
    io->iotd_Req.io_Command = CMD_WRITE;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { ok = FALSE; LogAdd("Write error"); break; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  Close(fh);
  CloseTD(p, io);
  return ok;
}

/* ----- Shutdown ----- */

static void CloseUI(void) {
  if (ui.win) {
    if (ui.gadlist) {
      RemoveGList(ui.win, ui.gadlist, (UWORD)-1);
      FreeGadgets(ui.gadlist);
      ui.gadlist = NULL;
    }
    CloseWindow(ui.win);
    ui.win = NULL;
  }
  if (ui.vi) {
    FreeVisualInfo(ui.vi);
    ui.vi = NULL;
  }
}

static void CloseAll(void) {
  CloseUI();
  if (GadToolsBase) CloseLibrary(GadToolsBase);
  if (AslBase)      CloseLibrary(AslBase);
  if (GfxBase)      CloseLibrary((struct Library*)GfxBase);
  if (IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
  if (DOSBase)      CloseLibrary((struct Library*)DOSBase);
}

/* ----- Helpers ----- */

static BOOL HasFile(CONST_STRPTR path) {
  BPTR lock = Lock((STRPTR)path, ACCESS_READ);
  if (lock) { UnLock(lock); return TRUE; }
  return FALSE;
}

/* Generate RAM:DF<unit>_<n>.adf with n from 0..999 ensuring file doesn't exist */
static BOOL GenUniqueAdfPath(UBYTE unit, char *out, int maxlen) {
  if (!out || maxlen < 10) return FALSE;
  for (int i=0; i<1000; ++i) {
    char name[64];
    sprintf(name, "RAM:DF%u_%03d.adf", (unsigned)unit, i);
    BPTR lock = Lock(name, ACCESS_READ);
    if (lock) { UnLock(lock); continue; }
    strncpy(out, name, maxlen-1);
    out[maxlen-1] = '\0';
    return TRUE;
  }
  return FALSE;
}

/* ----- CRC32 (poly 0xEDB88320) ----- */

static ULONG crc32_init(void) { return 0xFFFFFFFFUL; }

static ULONG crc32_update(ULONG crc, const UBYTE *buf, ULONG len) {
  ULONG c = crc;
  while (len--) {
    c ^= (ULONG)(*buf++);
    for (int k=0; k<8; ++k) {
      if (c & 1) c = (c >> 1) ^ 0xEDB88320UL;
      else       c = (c >> 1);
    }
  }
  return c;
}

static ULONG crc32_final(ULONG crc) { return crc ^ 0xFFFFFFFFUL; }
