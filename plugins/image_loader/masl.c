/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * The player of MASL scenarios (the .mas text, also inside .msl files),
 * after masmake2.doc of the MASL data development set (maslt120) and
 * masl.doc of MASL 1.4:
 *
 *   "MASL<version> <timer Hz>", comments up to a line starting with ".",
 *   the files (.ak5, .pkn) up to a line ".", an extension area up to a
 *   line ".", then the commands, apart by spaces, tabs or newlines.
 *
 * Not on a PC-9801, so:
 *   - time is virtual: the waits of the scenario ("t", "pwt") give it, so
 *     the timing it asks for is kept; drawing takes the time it really
 *     takes here (a benchmark like 森ベンチ measures this machine), the
 *     other commands a little (a loop of nothing but waiting goes on);
 *     a picture is taken at a wait, or after a 60th second of drawing;
 *   - the PCMs are told to a MaslSoundFunc (masl_sound.c plays them);
 *   - no keys, no options; a scenario waiting for a key with nothing
 *     changing for 3 seconds gets every key pressed for a moment (as
 *     "masl -xr" does, to play on);
 *   - the text screen (titles) is not shown; "out" does nothing.
 */

#include <math.h>
#include <string.h>

#include "masl.h"

#define MASL_PARAMS       8
#define MASL_REGS         256
#define MASL_CALL_DEPTH   64
#define MASL_CHANNELS     8

/* virtual time of a command (ms; drawing: the real time it takes) */
#define COST_INSN         0.01
#define FRAME_MS          (1000.0 / 60.0)
#define IDLE_KEY_MS       3000.0
#define KEY_PRESS_MS      100.0
#define IDLE_FORCE_MS     30000.0
#define BUDGET_INSNS      4000000

typedef enum {
   P_NUM, P_RAND, P_REG, P_REGREG, P_REGEXPR, P_KEYS, P_OPT, P_PCM, P_TIMER
} ParamKind;

typedef struct {
   guint8 kind;
   gint32 a, b, c;
} Param;

typedef enum {
   OP_NOP, OP_LOOP, OP_ENDLOOP, OP_IF, OP_ELSE, OP_ENDIF, OP_LABEL, OP_JUMP,
   OP_CALL, OP_RET, OP_END, OP_ASSIGN,
   OP_GOFF, OP_GON, OP_LINE4, OP_LINE2, OP_PW, OP_PD, OP_DRAW, OP_CLS,
   OP_BOX, OP_O2U, OP_U2O, OP_PAL, OP_MOVE, OP_ROLL, OP_PALSET, OP_TONE,
   OP_MASK, OP_PCM, OP_PWT, OP_PST, OP_RATE, OP_COUNT, OP_TIMER, OP_PMIX
} OpCode;

/* conditions: 0 none, '=', '!', '<', 'l' (<=), '>', 'g' (>=); loops also
 * 'n' (n times) */
typedef struct {
   guint8  op;
   guint8  cond;      /* or the operator of OP_ASSIGN */
   guint8  flags;     /* OP_TIMER: 1 clear before, 2 wait, 4 clear after;
                         OP_PCM: 1 loop; OP_END: 1 darkness given */
   guint8  n;         /* params */
   gint32  target;    /* the matching bracket */
   gint32  reg;       /* OP_ASSIGN */
   Param   p[MASL_PARAMS];
} Insn;

struct MaslPlayer_Tag {
   /* the program */
   GArray         *code;           /* Insn */
   gint32          labels[256];
   gdouble         hz0;
   const Ak5Entry *cgs;
   guint           n_cgs;
   const guchar  **palettes;
   guint           n_palettes;
   const MaslPcm  *pcms;
   guint           n_pcms;
   MaslSoundFunc   sound_func;
   gpointer        sound_data;

   /* the machine */
   guint           pc;
   gint16          regs[MASL_REGS];
   gint32         *counters;       /* of the "[n" loops */
   guint           calls[MASL_CALL_DEPTH];
   guint           n_calls;
   guint32         random;
   gboolean        ended;
   gboolean        end_shown;
   gint            end_tone;       /* -1: the screen cleared */

   Ak5Planes       page[2];
   guchar          color[4];
   gint            wpage, dpage;
   gboolean        gon, line2;
   guint           roll, roll_mode;
   gint            mask[4];
   guchar          pal[48];        /* 0..15 */
   gint            tone;

   gdouble         now;            /* ms */
   gdouble         hz;
   gdouble         tc_base;        /* ms of the timer's 0 */
   gdouble         pcm_end[MASL_CHANNELS];
   guint           rate;

   gboolean        dirty;          /* what is shown changed */
   gdouble         last_shot;
   gdouble         busy_since;     /* the last picture or wait */
   gdouble         keys_until;
};


/******************************************************************************
 *  parsing
 ******************************************************************************/

typedef struct {
   const gchar *s, *end;
} Src;

static inline gboolean
is_space (gchar c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0x1a;
}

static inline gboolean
at_end (Src *src)
{
   return src->s >= src->end;
}

static inline gchar
peek (Src *src, gint i)
{
   return src->s + i < src->end ? src->s[i] : '\0';
}

static gint32
parse_int (Src *src)
{
   gint32 v = 0;
   gboolean neg = FALSE;

   if (peek (src, 0) == '-') { neg = TRUE; src->s++; }
   while (g_ascii_isdigit (peek (src, 0))) {
      if (v < 100000000) v = v * 10 + (*src->s - '0');
      src->s++;
   }
   return neg ? -v : v;
}

static gboolean
param_start (gchar c)
{
   return g_ascii_isdigit (c) || c == '-' || c == '@' || c == 'r' || c == 'k'
      || c == 'o' || c == 'p' || c == 't';
}

static gboolean
parse_param (Src *src, Param *p)
{
   gchar c = peek (src, 0);

   memset (p, 0, sizeof (*p));
   if (g_ascii_isdigit (c) || (c == '-' && g_ascii_isdigit (peek (src, 1)))) {
      p->kind = P_NUM;
      p->a = parse_int (src);
   } else if (c == '@') {
      src->s++;
      if (peek (src, 0) == '@') {
         src->s++;
         p->kind = P_REGREG;
         p->a = parse_int (src);
      } else {
         p->kind = P_REG;
         p->a = parse_int (src);
         /* "@l[*m][+n]" (1.3T2) */
         if (peek (src, 0) == '*' || peek (src, 0) == '+') {
            p->kind = P_REGEXPR;
            p->b = 1;
            if (peek (src, 0) == '*') { src->s++; p->b = parse_int (src); }
            if (peek (src, 0) == '+') { src->s++; p->c = parse_int (src); }
         }
      }
   } else if (c == 'r') {
      src->s++;
      p->kind = P_RAND;
      p->a = parse_int (src);
   } else if (c == 'k') {
      src->s++;
      p->kind = P_KEYS;
      while (g_ascii_isalnum (peek (src, 0))) {
         if (*src->s == 'F' && g_ascii_isdigit (peek (src, 1))) src->s++;
         src->s++;
         p->a++;
      }
   } else if (c == 'o') {
      src->s++;
      p->kind = P_OPT;
      while (g_ascii_isalnum (peek (src, 0))) src->s++;
   } else if (c == 'p') {
      src->s++;
      p->kind = P_PCM;
   } else if (c == 't') {
      src->s++;
      p->kind = P_TIMER;
   } else {
      return FALSE;
   }
   return TRUE;
}

/* params apart by commas; returns the count */
static guint
parse_params (Src *src, Param *p, guint max)
{
   guint n = 0;

   if (!param_start (peek (src, 0))) return 0;
   while (n < max && parse_param (src, &p[n])) {
      n++;
      if (peek (src, 0) != ',') break;
      src->s++;
   }
   return n;
}

/* "=P1,P2" and the like */
static gboolean
parse_cond (Src *src, Insn *insn, guint at)
{
   gchar c = peek (src, 0);

   if (c != '=' && c != '!' && c != '<' && c != '>') return FALSE;
   src->s++;
   if ((c == '<' || c == '>') && peek (src, 0) == '=') {
      src->s++;
      c = c == '<' ? 'l' : 'g';
   }
   insn->cond = c;
   if (!parse_param (src, &insn->p[at])) return FALSE;
   if (peek (src, 0) == ',') src->s++;
   if (!parse_param (src, &insn->p[at + 1])) return FALSE;
   insn->n = at + 2;
   return TRUE;
}

static const struct { const gchar *name; OpCode op; gboolean space; } commands[] = {
   { "goff",  OP_GOFF,   FALSE },
   { "gon",   OP_GON,    FALSE },
   { "line4", OP_LINE4,  FALSE },
   { "line2", OP_LINE2,  FALSE },
   { "pal_",  OP_PALSET, FALSE },
   { "pmix",  OP_PMIX,   TRUE  },
   { "pal",   OP_PAL,    FALSE },
   { "pcm",   OP_PCM,    FALSE },
   { "pwt",   OP_PWT,    FALSE },
   { "pst",   OP_PST,    FALSE },
   { "pw",    OP_PW,     FALSE },
   { "pd",    OP_PD,     FALSE },
   { "cls",   OP_CLS,    FALSE },
   { "call",  OP_CALL,   FALSE },
   { "count", OP_COUNT,  FALSE },
   { "box",   OP_BOX,    TRUE  },
   { "o2u",   OP_O2U,    FALSE },
   { "u2o",   OP_U2O,    FALSE },
   { "move",  OP_MOVE,   TRUE  },
   { "mask",  OP_MASK,   TRUE  },
   { "roll",  OP_ROLL,   FALSE },
   { "rate",  OP_RATE,   FALSE },
   { "ret",   OP_RET,    FALSE },
   { "tone",  OP_TONE,   FALSE },
   { "out",   OP_NOP,    FALSE },
   { "err",   OP_NOP,    FALSE },
   { "d",     OP_DRAW,   FALSE },
   { "j",     OP_JUMP,   FALSE },
   { "t",     OP_TIMER,  FALSE },
};

/* one command at src (spaces skipped before); FALSE at the end */
static gboolean
parse_command (Src *src, Insn *insn)
{
   gchar c;
   guint i;

   memset (insn, 0, sizeof (*insn));
   insn->target = -1;

   for (;;) {
      while (!at_end (src) && is_space (*src->s)) src->s++;
      if (at_end (src)) return FALSE;
      if (*src->s != ';') break;
      while (!at_end (src) && *src->s != '\n') src->s++;
   }

   c = *src->s;
   switch (c) {
   case '[':
      src->s++;
      insn->op = OP_LOOP;
      if (g_ascii_isdigit (peek (src, 0))) {
         insn->cond = 'n';
         insn->p[0].kind = P_NUM;
         insn->p[0].a = parse_int (src);
         insn->n = 1;
      } else {
         parse_cond (src, insn, 0);
      }
      return TRUE;
   case ']':
      src->s++;
      insn->op = OP_ENDLOOP;
      parse_cond (src, insn, 0);
      return TRUE;
   case '{':
      src->s++;
      insn->op = OP_IF;
      parse_cond (src, insn, 0);
      return TRUE;
   case '}':
      src->s++;
      if (src->end - src->s >= 5 && !strncmp (src->s, "else{", 5)) {
         src->s += 5;
         insn->op = OP_ELSE;
      } else {
         insn->op = OP_ENDIF;
      }
      return TRUE;
   case '#':
      src->s++;
      insn->op = OP_LABEL;
      insn->reg = parse_int (src) & 255;
      return TRUE;
   case '\\':
      src->s++;
      insn->op = OP_END;
      if (g_ascii_isdigit (peek (src, 0))) {
         insn->flags = 1;
         insn->p[0].a = parse_int (src);
      }
      return TRUE;
   case '=': case '+': case '-': case '*': case '/': case '%':
   case '&': case '|': case '^':
      if (peek (src, 1) == '@') {
         src->s += 2;
         insn->op = OP_ASSIGN;
         insn->cond = c;
         insn->reg = parse_int (src) & 255;
         if (peek (src, 0) == ',') src->s++;
         if (parse_param (src, &insn->p[0])) insn->n = 1;
         return TRUE;
      }
      break;
   default:
      break;
   }

   for (i = 0; i < G_N_ELEMENTS (commands); i++) {
      gsize l = strlen (commands[i].name);
      if ((gsize) (src->end - src->s) < l || strncmp (src->s, commands[i].name, l))
         continue;
      src->s += l;
      insn->op = commands[i].op;

      switch (insn->op) {
      case OP_TIMER:
         if (peek (src, 0) == 'c') { src->s++; insn->flags |= 1; }
         if (param_start (peek (src, 0)) && parse_param (src, &insn->p[0])) {
            insn->flags |= 2;
            insn->n = 1;
         }
         if (peek (src, 0) == 'c') { src->s++; insn->flags |= 4; }
         break;
      case OP_JUMP:
      case OP_CALL:
         if (parse_cond (src, insn, 0)) {
            while (!at_end (src) && (*src->s == ' ' || *src->s == '\t' || *src->s == ','))
               src->s++;
            if (parse_param (src, &insn->p[2])) insn->n = 3;
         } else if (parse_param (src, &insn->p[0])) {
            insn->n = 1;
         }
         break;
      default:
         if (commands[i].space)
            while (!at_end (src) && (*src->s == ' ' || *src->s == '\t')) src->s++;
         insn->n = parse_params (src, insn->p, MASL_PARAMS);
         if (insn->op == OP_PCM && peek (src, 0) == '.') {
            src->s++;
            insn->flags |= 1;
         }
         break;
      }
      /* what follows unknown: up to the next space */
      return TRUE;
   }

   /* unknown: skipped */
   while (!at_end (src) && !is_space (*src->s)) src->s++;
   insn->op = OP_NOP;
   return TRUE;
}


/* the header: the version line, the comments, the files, the extension
 * area; returns where the commands start (NULL if not MASL) */
static const gchar *
parse_header (const gchar *s, const gchar *end, gdouble *hz)
{
   const gchar *line = s, *nl;
   gint periods = 0;
   gdouble nums[2] = { 0, 0 };
   gint n = 0;

   if (end - s < 4 || strncmp (s, "MASL", 4)) return NULL;

   /* the numbers of the first line: version, Hz */
   nl = memchr (s, '\n', end - s);
   if (!nl) return NULL;
   for (const gchar *p = s + 4; p < nl && n < 2; ) {
      if (g_ascii_isdigit (*p)) {
         gchar buf[32];
         gint l = 0;
         while (p < nl && (g_ascii_isdigit (*p) || *p == '.') && l < 31) buf[l++] = *p++;
         buf[l] = '\0';
         nums[n++] = g_ascii_strtod (buf, NULL);
      } else {
         p++;
      }
   }
   *hz = n == 2 && nums[1] > 0 ? nums[1] : 60.0;

   /* the comments end with a line starting with '.', the files and the
    * extension area each with a line of "." */
   line = nl + 1;
   while (line < end && periods < 3) {
      nl = memchr (line, '\n', end - line);
      if (!nl) nl = end;
      if (*line == '.') {
         if (periods == 0) {
            periods = 1;
         } else {
            const gchar *p = line + 1;
            while (p < nl && is_space (*p)) p++;
            if (p == nl) periods++;
         }
      }
      line = nl < end ? nl + 1 : end;
   }
   return periods == 3 ? line : NULL;
}


static gboolean
compile (MaslPlayer *pl, const gchar *s, const gchar *end)
{
   Src src = { s, end };
   Insn insn;
   GArray *stack = g_array_new (FALSE, FALSE, sizeof (gint32));
   guint i;

   for (i = 0; i < 256; i++) pl->labels[i] = -1;

   while (parse_command (&src, &insn)) {
      gint32 idx = pl->code->len;

      if (insn.op == OP_LOOP || insn.op == OP_IF) {
         g_array_append_val (stack, idx);
      } else if (insn.op == OP_ELSE) {
         if (stack->len) {
            gint32 open = g_array_index (stack, gint32, stack->len - 1);
            Insn *o = &g_array_index (pl->code, Insn, open);
            if (o->op == OP_IF) {
               o->target = idx;
               g_array_index (stack, gint32, stack->len - 1) = idx;
            }
         }
      } else if (insn.op == OP_ENDLOOP || insn.op == OP_ENDIF) {
         if (stack->len) {
            gint32 open = g_array_index (stack, gint32, stack->len - 1);
            Insn *o = &g_array_index (pl->code, Insn, open);
            g_array_set_size (stack, stack->len - 1);
            o->target = idx;
            insn.target = open;
         }
      } else if (insn.op == OP_LABEL) {
         pl->labels[insn.reg] = idx;
      }
      g_array_append_val (pl->code, insn);
   }
   g_array_free (stack, TRUE);
   pl->counters = g_new0 (gint32, pl->code->len + 1);
   return pl->code->len > 0;
}


/******************************************************************************
 *  the machine
 ******************************************************************************/

static inline gint
ticks (MaslPlayer *pl)
{
   return (gint) floor ((pl->now - pl->tc_base) * pl->hz / 1000.0 + 1e-6);
}

static gint
pcm_status (MaslPlayer *pl)
{
   gint i, bits = 0;
   for (i = 0; i < MASL_CHANNELS; i++)
      if (pl->pcm_end[i] > pl->now) bits |= 1 << i;
   return bits;
}

static gint
value (MaslPlayer *pl, const Param *p)
{
   switch (p->kind) {
   case P_NUM:
      return p->a;
   case P_RAND:
      pl->random = pl->random * 1103515245 + 12345;
      return p->a > 0 ? (gint) ((pl->random >> 16) % (guint) p->a) : 0;
   case P_REG:
      return pl->regs[p->a & 255];
   case P_REGREG:
      return pl->regs[pl->regs[p->a & 255] & 255];
   case P_REGEXPR:
      return (gint16) ((gint64) pl->regs[p->a & 255] * p->b + p->c);
   case P_KEYS:
      return pl->now < pl->keys_until ? (gint) ((1u << MIN (p->a, 15)) - 1) : 0;
   case P_OPT:
      return 0;
   case P_PCM:
      return pcm_status (pl);
   case P_TIMER:
      return ticks (pl);
   default:
      return 0;
   }
}

static inline gint
arg (MaslPlayer *pl, const Insn *insn, guint i, gint def)
{
   return i < insn->n ? value (pl, &insn->p[i]) : def;
}

static gboolean
cond_true (MaslPlayer *pl, const Insn *insn, guint at)
{
   gint a = (gint16) value (pl, &insn->p[at]);
   gint b = (gint16) value (pl, &insn->p[at + 1]);

   switch (insn->cond) {
   case '=': return a == b;
   case '!': return a != b;
   case '<': return a < b;
   case 'l': return a <= b;
   case '>': return a > b;
   case 'g': return a >= b;
   default:  return TRUE;
   }
}


static void
render (MaslPlayer *pl, guchar *rgb, gint tone)
{
   guchar (*pg)[AK5_VRAM] = pl->page[pl->dpage];
   guchar colors[16][3];
   guint wrap = pl->roll_mode == 1 ? AK5_VRAM : pl->roll_mode == 2 ? 408 * AK5_LINE : 400 * AK5_LINE;
   guint start = (pl->roll * 2) % wrap;
   gint x, y, i;

   for (i = 0; i < 16; i++) {
      gint k;
      for (k = 0; k < 3; k++)
         colors[i][k] = MIN (15, pl->pal[i * 3 + k] * tone / 100) * 17;
   }

   for (y = 0; y < AK5_HEIGHT; y++) {
      gint vy = pl->line2 ? y / 2 : y;
      gboolean row_in = pl->gon && y >= pl->mask[1] && y <= pl->mask[3];
      for (x = 0; x < AK5_WIDTH; x++, rgb += 3) {
         guint o, s, c;
         if (!row_in || x < pl->mask[0] || x > pl->mask[2]) {
            rgb[0] = rgb[1] = rgb[2] = 0;
            continue;
         }
         o = (start + vy * AK5_LINE + (x >> 3)) % wrap & (AK5_VRAM - 1);
         s = 7 - (x & 7);
         c = ((pg[0][o] >> s) & 1)
           | (((pg[1][o] >> s) & 1) << 1)
           | (((pg[2][o] >> s) & 1) << 2)
           | (((pg[3][o] >> s) & 1) << 3);
         rgb[0] = colors[c][0];
         rgb[1] = colors[c][1];
         rgb[2] = colors[c][2];
      }
   }
}


static void
set_palette (MaslPlayer *pl, gint n)
{
   gint i;
   if (n < 0 || n >= (gint) pl->n_palettes) return;
   for (i = 0; i < 48; i++) pl->pal[i] = pl->palettes[n][i] & 0x0f;
}


static void
box (MaslPlayer *pl, gint x1, gint y1, gint x2, gint y2, const gint v[4])
{
   gint x, y, k;

   x1 = CLAMP (x1, 0, AK5_WIDTH - 1);  x2 = CLAMP (x2, 0, AK5_WIDTH - 1);
   y1 = CLAMP (y1, 0, AK5_HEIGHT - 1); y2 = CLAMP (y2, 0, AK5_HEIGHT - 1);
   if (x1 > x2 || y1 > y2) return;
   for (y = y1; y <= y2; y++) {
      for (x = x1 >> 3; x <= x2 >> 3; x++) {
         guint o = y * AK5_LINE + x;
         gint b0 = MAX (x * 8, x1) - x * 8, b1 = MIN (x * 8 + 7, x2) - x * 8;
         guchar m = (guchar) ((0xff >> b0) & (0xff << (7 - b1)));
         for (k = 0; k < 4; k++)
            pl->page[pl->wpage][k][o] = (pl->page[pl->wpage][k][o] & ~m) | (v[k] & m);
      }
   }
}


static void
move (MaslPlayer *pl, gint x1, gint y1, gint x2, gint y2, gint x3, gint y3)
{
   gint w, h, y, k;
   guchar *tmp;

   x1 = CLAMP (x1, 0, AK5_LINE - 1);   x2 = CLAMP (x2, 0, AK5_LINE - 1);
   y1 = CLAMP (y1, 0, AK5_HEIGHT - 1); y2 = CLAMP (y2, 0, AK5_HEIGHT - 1);
   if (x1 > x2 || y1 > y2) return;
   w = x2 - x1 + 1;
   h = y2 - y1 + 1;
   tmp = g_malloc ((gsize) w * h * 4);
   for (k = 0; k < 4; k++)
      for (y = 0; y < h; y++)
         memcpy (tmp + (k * h + y) * w, pl->page[pl->wpage][k] + (y1 + y) * AK5_LINE + x1, w);
   for (k = 0; k < 4; k++)
      for (y = 0; y < h; y++) {
         gint dy = y3 + y, x;
         if (dy < 0 || dy >= AK5_HEIGHT) continue;
         for (x = 0; x < w; x++) {
            gint dx = x3 + x;
            if (dx < 0 || dx >= AK5_LINE) continue;
            pl->page[pl->wpage][k][dy * AK5_LINE + dx] = tmp[(k * h + y) * w + x];
         }
      }
   g_free (tmp);
}


/* the work takes time; a picture after a 60th second of it */
static gboolean
spend (MaslPlayer *pl, gdouble ms)
{
   pl->now += ms;
   return pl->dirty && pl->now - pl->last_shot >= FRAME_MS;
}


/* drawing since t0 (g_get_monotonic_time ()) took its real time */
static gboolean
spend_real (MaslPlayer *pl, gint64 t0)
{
   return spend (pl, MAX (COST_INSN, (g_get_monotonic_time () - t0) / 1000.0));
}


/* a wait up to "until": a picture first if something changed */
static gboolean
wait_until (MaslPlayer *pl, gdouble until, gboolean *shot)
{
   if (until <= pl->now) return FALSE;
   if (pl->dirty) {
      *shot = TRUE;
      return TRUE;        /* the wait again after the picture */
   }
   pl->now = until;
   pl->busy_since = until;
   return FALSE;
}


/* runs one command; TRUE when a picture is to be taken (the command may
 * be run again after it: pc not moved) */
static gboolean
step (MaslPlayer *pl)
{
   Insn *insn;
   gboolean shot = FALSE;
   guint next;
   gint64 t0 = g_get_monotonic_time ();

   if (pl->pc >= pl->code->len) {
      pl->ended = TRUE;
      pl->end_tone = 100;
      return TRUE;
   }
   insn = &g_array_index (pl->code, Insn, pl->pc);
   next = pl->pc + 1;

   switch (insn->op) {
   case OP_LOOP:
      if (insn->cond == 'n') {
         pl->counters[pl->pc] = arg (pl, insn, 0, 0);
         if (pl->counters[pl->pc] <= 0 && insn->target >= 0) next = insn->target + 1;
      } else if (insn->cond && !cond_true (pl, insn, 0) && insn->target >= 0) {
         next = insn->target + 1;
      }
      break;
   case OP_ENDLOOP: {
      Insn *open;
      if (insn->target < 0) break;
      open = &g_array_index (pl->code, Insn, insn->target);
      if (open->cond == 'n') {
         if (--pl->counters[insn->target] > 0) next = insn->target + 1;
      } else if (open->cond) {
         next = insn->target;
      } else if (insn->cond) {
         if (cond_true (pl, insn, 0)) next = insn->target + 1;
      } else {
         next = insn->target + 1;
      }
      break;
   }
   case OP_IF:
      if (!cond_true (pl, insn, 0) && insn->target >= 0) next = insn->target + 1;
      break;
   case OP_ELSE:
      if (insn->target >= 0) next = insn->target + 1;
      break;
   case OP_JUMP:
   case OP_CALL: {
      gint label;
      if (insn->n == 3) {
         if (!cond_true (pl, insn, 0)) break;
         label = value (pl, &insn->p[2]);
      } else {
         label = arg (pl, insn, 0, 0);
      }
      if (label < 0 || label > 255 || pl->labels[label] < 0) break;
      if (insn->op == OP_CALL) {
         if (pl->n_calls >= MASL_CALL_DEPTH) break;
         pl->calls[pl->n_calls++] = next;
      }
      next = pl->labels[label];
      break;
   }
   case OP_RET:
      if (pl->n_calls) next = pl->calls[--pl->n_calls];
      break;
   case OP_END:
      pl->ended = TRUE;
      pl->end_tone = insn->flags ? CLAMP (insn->p[0].a, 0, 100) : -1;
      return TRUE;
   case OP_ASSIGN: {
      gint64 a = pl->regs[insn->reg], b = arg (pl, insn, 0, 0);
      switch (insn->cond) {
      case '=': a = b; break;
      case '+': a += b; break;
      case '-': a -= b; break;
      case '*': a *= b; break;
      case '/': if (b) a /= b; break;
      case '%': if (b) a %= b; break;
      case '&': a &= b; break;
      case '|': a |= b; break;
      case '^': a ^= b; break;
      }
      pl->regs[insn->reg] = (gint16) a;
      break;
   }
   case OP_GOFF:
      if (pl->gon) pl->dirty = TRUE;
      pl->gon = FALSE;
      break;
   case OP_GON:
      if (!pl->gon) pl->dirty = TRUE;
      pl->gon = TRUE;
      break;
   case OP_LINE4:
   case OP_LINE2:
      pl->line2 = insn->op == OP_LINE2;
      pl->dirty = TRUE;
      break;
   case OP_PW:
      pl->wpage = arg (pl, insn, 0, 0) & 1;
      break;
   case OP_PD: {
      gint p = arg (pl, insn, 0, 0) & 1;
      if (p != pl->dpage) pl->dirty = TRUE;
      pl->dpage = p;
      break;
   }
   case OP_DRAW: {
      gint n = arg (pl, insn, 0, 0), x = arg (pl, insn, 1, 0), y = arg (pl, insn, 2, 0);
      const Ak5Entry *e;
      if (n < 0 || n >= (gint) pl->n_cgs) break;
      e = &pl->cgs[n];
      ak5_decode_cg (pl->page[pl->wpage], pl->color, e->data, e->len,
                     (guint) (y * AK5_LINE + x) & 0xffff);
      if (pl->wpage == pl->dpage) pl->dirty = TRUE;
      shot = spend_real (pl, t0);
      break;
   }
   case OP_CLS:
      memset (pl->page[pl->wpage], 0, sizeof (pl->page[0]));
      if (pl->wpage == pl->dpage) pl->dirty = TRUE;
      shot = spend_real (pl, t0);
      break;
   case OP_BOX: {
      gint v[4] = { arg (pl, insn, 4, 0), arg (pl, insn, 5, 0),
                    arg (pl, insn, 6, 0), arg (pl, insn, 7, 0) };
      box (pl, arg (pl, insn, 0, 0), arg (pl, insn, 1, 0),
           arg (pl, insn, 2, 0), arg (pl, insn, 3, 0), v);
      if (pl->wpage == pl->dpage) pl->dirty = TRUE;
      shot = spend_real (pl, t0);
      break;
   }
   case OP_O2U:
   case OP_U2O: {
      gint from = insn->op == OP_O2U ? 0 : 1;
      memcpy (pl->page[!from], pl->page[from], sizeof (pl->page[0]));
      if (pl->dpage == !from) pl->dirty = TRUE;
      shot = spend_real (pl, t0);
      break;
   }
   case OP_MOVE:
      move (pl, arg (pl, insn, 0, 0), arg (pl, insn, 1, 0), arg (pl, insn, 2, 0),
            arg (pl, insn, 3, 0), arg (pl, insn, 4, 0), arg (pl, insn, 5, 0));
      if (pl->wpage == pl->dpage) pl->dirty = TRUE;
      shot = spend_real (pl, t0);
      break;
   case OP_ROLL:
      pl->roll = (guint) arg (pl, insn, 0, 0) & 0xffff;
      pl->roll_mode = (guint) arg (pl, insn, 1, 0) % 3;
      pl->dirty = TRUE;
      break;
   case OP_PAL:
      set_palette (pl, arg (pl, insn, 0, 0));
      pl->tone = CLAMP (arg (pl, insn, 1, 100), 0, 100);
      pl->dirty = TRUE;
      break;
   case OP_PALSET: {
      gint n = arg (pl, insn, 0, 0);
      if (n >= 0 && n < 16) {
         pl->pal[n * 3]     = arg (pl, insn, 1, 0) & 0x0f;
         pl->pal[n * 3 + 1] = arg (pl, insn, 2, 0) & 0x0f;
         pl->pal[n * 3 + 2] = arg (pl, insn, 3, 0) & 0x0f;
      }
      pl->dirty = TRUE;
      break;
   }
   case OP_PMIX: {
      gint a = arg (pl, insn, 0, 0), b = arg (pl, insn, 1, 0);
      gint r = CLAMP (arg (pl, insn, 2, 100), 0, 100), i;
      if (a >= 0 && a < (gint) pl->n_palettes && b >= 0 && b < (gint) pl->n_palettes)
         for (i = 0; i < 48; i++)
            pl->pal[i] = ((pl->palettes[a][i] & 0x0f) * r
                          + (pl->palettes[b][i] & 0x0f) * (100 - r)) / 100;
      pl->tone = CLAMP (arg (pl, insn, 3, 100), 0, 100);
      pl->dirty = TRUE;
      break;
   }
   case OP_TONE:
      pl->tone = CLAMP (arg (pl, insn, 0, 100), 0, 100);
      pl->dirty = TRUE;
      break;
   case OP_MASK:
      pl->mask[0] = arg (pl, insn, 0, 0);
      pl->mask[1] = arg (pl, insn, 1, 0);
      pl->mask[2] = arg (pl, insn, 2, AK5_WIDTH - 1);
      pl->mask[3] = arg (pl, insn, 3, AK5_HEIGHT - 1);
      pl->dirty = TRUE;
      break;
   case OP_PCM: {
      gint n = arg (pl, insn, 0, 0), ch = arg (pl, insn, 1, -1), i;
      const MaslPcm *pcm;
      gdouble samples, rate;
      if (n < 0 || n >= (gint) pl->n_pcms) break;
      pcm = &pl->pcms[n];
      if (ch < 0)
         for (i = 0; i < MASL_CHANNELS; i++)
            if (pl->pcm_end[i] <= pl->now) { ch = i; break; }
      if (ch < 0 || ch >= MASL_CHANNELS) ch = 0;
      samples = pcm->adpcm ? pcm->len * 2.0 : (gdouble) pcm->len;
      rate = pl->rate ? pl->rate : pcm->rate ? pcm->rate : 16000;
      pl->pcm_end[ch] = (insn->flags & 1) ? INFINITY : pl->now + samples * 1000.0 / rate;
      if (pl->sound_func)
         pl->sound_func (MASL_SOUND_PLAY, ch, n, (guint) rate, insn->flags & 1,
                         pl->now, pl->sound_data);
      break;
   }
   case OP_PWT: {
      gint bits = arg (pl, insn, 0, 255), i;
      gdouble until = pl->now;
      for (i = 0; i < MASL_CHANNELS; i++)
         if ((bits & (1 << i)) && isfinite (pl->pcm_end[i]) && pl->pcm_end[i] > until)
            until = pl->pcm_end[i];
      if (wait_until (pl, until, &shot)) return TRUE;
      break;
   }
   case OP_PST: {
      gint bits = arg (pl, insn, 0, 255), i;
      for (i = 0; i < MASL_CHANNELS; i++)
         if (bits & (1 << i)) pl->pcm_end[i] = 0;
      if (pl->sound_func)
         pl->sound_func (MASL_SOUND_STOP, bits, -1, 0, FALSE, pl->now, pl->sound_data);
      break;
   }
   case OP_RATE: {
      gint i;
      pl->rate = (guint) MAX (arg (pl, insn, 0, 0), 0);
      for (i = 0; i < MASL_CHANNELS; i++) pl->pcm_end[i] = 0;
      if (pl->sound_func)
         pl->sound_func (MASL_SOUND_STOP, 0xff, -1, 0, FALSE, pl->now, pl->sound_data);
      break;
   }
   case OP_COUNT: {
      gdouble hz = arg (pl, insn, 0, 0) / 10.0;
      if (hz > 0) {
         /* the count so far kept */
         gdouble t = (pl->now - pl->tc_base) * pl->hz / 1000.0;
         pl->hz = hz;
         pl->tc_base = pl->now - t * 1000.0 / hz;
      }
      break;
   }
   case OP_TIMER:
      /* "tc": clear, "tP": wait until P, "tcP", "tPc" both */
      if ((insn->flags & 1) && !(insn->flags & 8)) {
         pl->tc_base = pl->now;
         insn->flags |= 8;          /* done (the command may run again) */
      }
      if (insn->flags & 2) {
         gdouble until = pl->tc_base + value (pl, &insn->p[0]) * 1000.0 / pl->hz;
         if (wait_until (pl, until, &shot)) return TRUE;
      }
      insn->flags &= ~8;
      if (insn->flags & 4) pl->tc_base = pl->now;
      break;
   default:
      break;
   }

   pl->pc = next;
   if (!shot) shot = spend (pl, COST_INSN);
   return shot;
}


static void
machine_reset (MaslPlayer *pl)
{
   guint i;

   pl->pc = 0;
   memset (pl->regs, 0, sizeof (pl->regs));
   memset (pl->counters, 0, sizeof (gint32) * (pl->code->len + 1));
   pl->n_calls = 0;
   pl->random = 1;
   pl->ended = pl->end_shown = FALSE;
   pl->end_tone = 100;
   memset (pl->page, 0, sizeof (pl->page));
   memset (pl->color, 0, sizeof (pl->color));
   pl->wpage = pl->dpage = 0;
   pl->gon = TRUE;
   pl->line2 = FALSE;
   pl->roll = pl->roll_mode = 0;
   pl->mask[0] = pl->mask[1] = 0;
   pl->mask[2] = AK5_WIDTH - 1;
   pl->mask[3] = AK5_HEIGHT - 1;
   for (i = 0; i < 16; i++) {
      pl->pal[i * 3]     = (i & 2) ? 15 : 0;
      pl->pal[i * 3 + 1] = (i & 4) ? 15 : 0;
      pl->pal[i * 3 + 2] = (i & 1) ? 15 : 0;
   }
   pl->tone = 100;
   pl->now = pl->tc_base = 0;
   pl->hz = pl->hz0;
   for (i = 0; i < MASL_CHANNELS; i++) pl->pcm_end[i] = 0;
   pl->rate = 0;
   pl->dirty = TRUE;
   pl->last_shot = -FRAME_MS;
   pl->busy_since = 0;
   pl->keys_until = -1;
   for (i = 0; i < pl->code->len; i++)
      g_array_index (pl->code, Insn, i).flags &= ~8;
}


/******************************************************************************
 *  public
 ******************************************************************************/

MaslPlayer *
masl_player_new (const guchar *scenario, gsize len,
                 const Ak5Entry *cgs, guint n_cgs,
                 const guchar **palettes, guint n_palettes,
                 const MaslPcm *pcms, guint n_pcms)
{
   MaslPlayer *pl;
   const gchar *s = (const gchar *) scenario, *end = s + len, *cmds;
   gdouble hz;

   cmds = parse_header (s, end, &hz);
   if (!cmds) return NULL;

   pl = g_new0 (MaslPlayer, 1);
   pl->code = g_array_new (FALSE, FALSE, sizeof (Insn));
   pl->hz0 = hz;
   pl->cgs = cgs;
   pl->n_cgs = n_cgs;
   pl->palettes = palettes;
   pl->n_palettes = n_palettes;
   pl->pcms = pcms;
   pl->n_pcms = n_pcms;

   if (!compile (pl, cmds, end)) {
      masl_player_free (pl);
      return NULL;
   }
   machine_reset (pl);
   return pl;
}


void
masl_player_free (MaslPlayer *pl)
{
   if (!pl) return;
   if (pl->code) g_array_free (pl->code, TRUE);
   g_free (pl->counters);
   g_free (pl);
}


void
masl_player_set_sound_func (MaslPlayer *pl, MaslSoundFunc func, gpointer data)
{
   g_return_if_fail (pl);
   pl->sound_func = func;
   pl->sound_data = data;
}


void
masl_player_reset (MaslPlayer *pl)
{
   g_return_if_fail (pl);
   machine_reset (pl);
}


gboolean
masl_player_next (MaslPlayer *pl, guchar *rgb, gdouble *time_ms)
{
   guint budget = BUDGET_INSNS;

   g_return_val_if_fail (pl, FALSE);

   if (pl->end_shown) return FALSE;

   for (;;) {
      gboolean shot;

      if (pl->ended) break;

      shot = step (pl);
      if (pl->ended) break;
      if (shot) break;

      /* nothing changing: a key wait? */
      if (!pl->dirty && pl->now - pl->busy_since > IDLE_KEY_MS
          && pl->now >= pl->keys_until + IDLE_KEY_MS)
         pl->keys_until = pl->now + KEY_PRESS_MS;
      if (pl->now - pl->last_shot > IDLE_FORCE_MS || --budget == 0) break;
   }

   if (pl->ended) {
      /* the last picture: dark by the scenario, or cleared */
      pl->end_shown = TRUE;
      if (pl->end_tone < 0) {
         memset (pl->page[pl->dpage], 0, sizeof (pl->page[0]));
         pl->end_tone = 100;
      }
      render (pl, rgb, pl->tone * pl->end_tone / 100);
   } else {
      render (pl, rgb, pl->tone);
   }
   *time_ms = pl->now;
   pl->last_shot = pl->busy_since = pl->now;
   pl->dirty = FALSE;
   return TRUE;
}
