#define _GNU_SOURCE
#include <locale.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#define CMD_BUF 2048
#define OUT_CHUNK 4096

static int R, C;

/* ---------- colori ---------- */
enum {
  CP_DEFAULT = 1,
  CP_OK,
  CP_WARN,
  CP_ERR,
  CP_TITLE,
  CP_HILITE
};

static void init_colors(void) {
  if (!has_colors()) return;
  start_color();
  use_default_colors();
  init_pair(CP_DEFAULT, -1, -1);
  init_pair(CP_OK, COLOR_GREEN, -1);
  init_pair(CP_WARN, COLOR_YELLOW, -1);
  init_pair(CP_ERR, COLOR_RED, -1);
  init_pair(CP_TITLE, COLOR_CYAN, -1);
  init_pair(CP_HILITE, COLOR_BLACK, COLOR_CYAN);
}

static void trim(char *s) {
  size_t n = strlen(s);
  while (n && (s[n-1] == '\n' || s[n-1] == '\r' || isspace((unsigned char)s[n-1]))) s[--n] = 0;
  size_t i = 0;
  while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s+i, strlen(s+i)+1);
}

static bool is_ipv4(const char *s) {
  int a,b,c,d; char tail;
  if (sscanf(s, "%d.%d.%d.%d%c", &a,&b,&c,&d,&tail) != 4) return false;
  if (a<0||a>255||b<0||b>255||c<0||c>255||d<0||d>255) return false;
  return true;
}

/* ---------- esegui comando e cattura output ---------- */
static char *run_cmd_capture(const char *cmd, int *exit_code) {
  char full[CMD_BUF];
  snprintf(full, sizeof(full), "%s 2>&1", cmd);

  FILE *fp = popen(full, "r");
  if (!fp) {
    if (exit_code) *exit_code = 127;
    return strdup("Errore: popen() fallita\n");
  }

  size_t cap = OUT_CHUNK, len = 0;
  char *buf = malloc(cap);
  if (!buf) { pclose(fp); return strdup("Errore: malloc fallita\n"); }
  buf[0] = 0;

  char chunk[OUT_CHUNK];
  while (fgets(chunk, sizeof(chunk), fp)) {
    size_t clen = strlen(chunk);
    if (len + clen + 1 > cap) {
      cap = (cap + clen + 1) * 2;
      char *nb = realloc(buf, cap);
      if (!nb) { free(buf); pclose(fp); return strdup("Errore: realloc fallita\n"); }
      buf = nb;
    }
    memcpy(buf + len, chunk, clen);
    len += clen;
    buf[len] = 0;
  }

  int st = pclose(fp);
  int code = 0;
  if (WIFEXITED(st)) code = WEXITSTATUS(st);
  else code = 128;
  if (exit_code) *exit_code = code;
  return buf;
}

/* ---------- cornice compatibile (ACS) ---------- */
static void box_compat(WINDOW *w) {
  // Questa usa i line-drawing di ncurses (ACS) -> compatibile Terminator
  wborder(w, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE,
            ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
}

/* ---------- prompt input in finestra centrata ---------- */
static bool prompt_input(const char *titolo, const char *etichetta, char *out, size_t out_sz) {
  int win_w = (C < 70) ? C-4 : 70;
  int win_h = 9;
  int y = (R - win_h) / 2;
  int x = (C - win_w) / 2;

  WINDOW *w = newwin(win_h, win_w, y, x);
  if (!w) return false;

  wbkgd(w, COLOR_PAIR(CP_DEFAULT));
  box_compat(w);

  wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
  mvwprintw(w, 1, 3, "%s", titolo);
  wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);

  mvwprintw(w, 3, 3, "%s", etichetta);
  mvwprintw(w, 5, 3, "> ");

  wrefresh(w);

  echo();
  curs_set(1);
  mvwgetnstr(w, 5, 5, out, (int)out_sz-1);
  noecho();
  curs_set(0);

  delwin(w);

  trim(out);
  return strlen(out) > 0;
}

/* ---------- pager con scrolling ---------- */
typedef struct { char **lines; size_t nlines; } Lines;

static Lines split_lines(const char *text) {
  Lines L = {0};
  if (!text) return L;

  size_t cnt = 1;
  for (const char *p=text; *p; p++) if (*p=='\n') cnt++;

  char **tmp = calloc(cnt, sizeof(char*));
  if (!tmp) return L;

  char *copy = strdup(text);
  if (!copy) { free(tmp); return L; }

  size_t idx=0;
  char *save=NULL;
  char *line=strtok_r(copy, "\n", &save);
  while (line && idx<cnt) { tmp[idx++] = line; line=strtok_r(NULL,"\n",&save); }

  L.lines = calloc(idx ? idx : 1, sizeof(char*));
  if (!L.lines) { free(copy); free(tmp); return (Lines){0}; }
  for (size_t i=0;i<idx;i++) L.lines[i] = strdup(tmp[i] ? tmp[i] : "");
  L.nlines = idx;

  free(copy);
  free(tmp);
  return L;
}

static void free_lines(Lines *L) {
  if (!L || !L->lines) return;
  for (size_t i=0;i<L->nlines;i++) free(L->lines[i]);
  free(L->lines);
  L->lines=NULL; L->nlines=0;
}

static void pager_show(const char *titolo, const char *contenuto) {
  Lines L = split_lines(contenuto ? contenuto : "");
  size_t off=0;

  int top=2, bottom=R-3;
  int height=bottom-top+1;

  while (1) {
    clear();

    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvhline(0,0,' ',C);
    mvprintw(0,2,"%s", titolo);
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvhline(1,0,ACS_HLINE,C);
    mvhline(R-2,0,ACS_HLINE,C);

    attron(COLOR_PAIR(CP_WARN));
    mvprintw(R-1,2,"↑↓ PgUp PgDn scorri | q indietro");
    attroff(COLOR_PAIR(CP_WARN));

    for (int i=0;i<height;i++) {
      size_t li = off + (size_t)i;
      if (li >= L.nlines) break;
      mvprintw(top+i, 0, "%.*s", C-1, L.lines[li]);
    }

    refresh();
    int ch=getch();
    if (ch=='q' || ch==27) break;
    if (ch==KEY_DOWN) { if (off+1 < L.nlines) off++; }
    else if (ch==KEY_UP) { if (off>0) off--; }
    else if (ch==KEY_NPAGE) {
      size_t n = off + (size_t)height;
      off = (n < L.nlines) ? n : (L.nlines ? L.nlines-1 : 0);
    } else if (ch==KEY_PPAGE) {
      off = (off > (size_t)height) ? off - (size_t)height : 0;
    } else if (ch==KEY_RESIZE) {
      getmaxyx(stdscr,R,C);
      top=2; bottom=R-3; height=bottom-top+1;
    }
  }

  free_lines(&L);
}

/* ---------- header ---------- */
static void get_hostname(char *buf, size_t n) {
  if (gethostname(buf, n) != 0) snprintf(buf, n, "unknown");
  buf[n-1]=0;
}

static void get_primary_ip(char *buf, size_t n) {
  int code=0;
  char *out = run_cmd_capture("ip -br a", &code);
  buf[0]=0;
  if (out) {
    Lines L = split_lines(out);
    for (size_t i=0;i<L.nlines;i++) {
      const char *ln = L.lines[i];
      if (!ln) continue;
      if (!strstr(ln, " UP ")) continue;
      const char *p = ln;
      while (*p) {
        if (isdigit((unsigned char)*p)) {
          char tmp[64]={0}; int j=0;
          while (*p && !isspace((unsigned char)*p) && j<63) tmp[j++]=*p++;
          tmp[j]=0;
          char *slash=strchr(tmp,'/'); if (slash) *slash=0;
          if (is_ipv4(tmp)) { snprintf(buf,n,"%s",tmp); break; }
        } else p++;
      }
      if (buf[0]) break;
    }
    free_lines(&L);
    free(out);
  }
  if (!buf[0]) snprintf(buf,n,"-");
}

static void draw_topbar(void) {
  char host[128]; get_hostname(host,sizeof(host));
  char ip[64]; get_primary_ip(ip,sizeof(ip));
  time_t t=time(NULL);
  struct tm tm; localtime_r(&t,&tm);
  char ts[64]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tm);

  attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
  mvhline(0,0,' ',C);
  mvprintw(0,2,"nft-tui");
  attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

  attron(COLOR_PAIR(CP_WARN));
  mvprintw(0, 12, "host:%s  ip:%s  %s", host, ip, ts);
  attroff(COLOR_PAIR(CP_WARN));

  mvhline(1,0,ACS_HLINE,C);
}

/* ---------- menu centrato con cornice ACS ---------- */
static int menu_centrato(const char *titolo, const char **voci, int n) {
  int maxlen = (int)strlen(titolo);
  for (int i=0;i<n;i++) {
    int l=(int)strlen(voci[i]);
    if (l>maxlen) maxlen=l;
  }

  int win_w = maxlen + 10;
  if (win_w < 46) win_w = 46;
  if (win_w > C-4) win_w = C-4;

  int win_h = n + 7;
  if (win_h < 12) win_h = 12;
  if (win_h > R-4) win_h = R-4;

  int y = (R - win_h) / 2;
  int x = (C - win_w) / 2;

  WINDOW *w = newwin(win_h, win_w, y, x);
  if (!w) return -1;

  keypad(w, TRUE);
  int sel=0;

  while (1) {
    werase(w);
    wbkgd(w, COLOR_PAIR(CP_DEFAULT));
    box_compat(w);

    wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(w, 1, 3, "%s", titolo);
    wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);

    wattron(w, COLOR_PAIR(CP_WARN));
    mvwprintw(w, win_h-2, 3, "Invio seleziona | q indietro");
    wattroff(w, COLOR_PAIR(CP_WARN));

    int start_y = 3;
    for (int i=0;i<n;i++) {
      if (i==sel) wattron(w, COLOR_PAIR(CP_HILITE) | A_BOLD);
      mvwprintw(w, start_y+i, 3, "%s", voci[i]);
      if (i==sel) wattroff(w, COLOR_PAIR(CP_HILITE) | A_BOLD);
    }

    wrefresh(w);

    int ch = wgetch(w);
    if (ch=='q' || ch==27) { delwin(w); return -1; }
    if (ch==KEY_UP) { if (sel>0) sel--; }
    else if (ch==KEY_DOWN) { if (sel<n-1) sel++; }
    else if (ch==10 || ch==KEY_ENTER) { delwin(w); return sel; }
    else if (ch==KEY_RESIZE) {
      getmaxyx(stdscr, R, C);
      delwin(w);
      return menu_centrato(titolo, voci, n);
    }
  }
}

/* ---------- gestione set (blacklist/whitelist) ---------- */
static void gestisci_set(const char *nome_set) {
  const char *menu[] = {
    "Visualizza set",
    "Aggiungi IP",
    "Rimuovi IP",
    "Svuota set",
    "Indietro"
  };

  while (1) {
    char titolo[256];
    snprintf(titolo, sizeof(titolo), "Gestione set: %s", nome_set);

    int sel = menu_centrato(titolo, menu, 5);
    if (sel < 0 || sel == 4) return;

    if (sel == 0) {
      char cmd[CMD_BUF];
      snprintf(cmd, sizeof(cmd), "nft list set inet filter %s", nome_set);
      char *out = run_cmd_capture(cmd, NULL);
      pager_show(titolo, out);
      free(out);
    } else if (sel == 1) {
      char ip[128]={0};
      if (!prompt_input("Aggiungi IP", "Inserisci IPv4:", ip, sizeof(ip))) continue;
      if (!is_ipv4(ip)) { pager_show("Errore", "IPv4 non valido"); continue; }

      char cmd[CMD_BUF];
      snprintf(cmd, sizeof(cmd), "nft add element inet filter %s { %s }", nome_set, ip);
      int code=0; char *out=run_cmd_capture(cmd,&code);
      pager_show(code==0 ? "OK" : "Errore", out);
      free(out);
    } else if (sel == 2) {
      char ip[128]={0};
      if (!prompt_input("Rimuovi IP", "Inserisci IPv4:", ip, sizeof(ip))) continue;
      if (!is_ipv4(ip)) { pager_show("Errore", "IPv4 non valido"); continue; }

      char cmd[CMD_BUF];
      snprintf(cmd, sizeof(cmd), "nft delete element inet filter %s { %s }", nome_set, ip);
      int code=0; char *out=run_cmd_capture(cmd,&code);
      pager_show(code==0 ? "OK" : "Errore", out);
      free(out);
    } else if (sel == 3) {
      char cmd[CMD_BUF];
      snprintf(cmd, sizeof(cmd), "nft flush set inet filter %s", nome_set);
      int code=0; char *out=run_cmd_capture(cmd,&code);
      pager_show(code==0 ? "OK" : "Errore", out);
      free(out);
    }
  }
}

int main(void) {
  if (geteuid() != 0) {
    fprintf(stderr, "Esegui con sudo.\n");
    return 1;
  }

  // anche se usiamo ACS, impostare locale aiuta coi terminali
  setlocale(LC_ALL, "");

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  getmaxyx(stdscr, R, C);
  init_colors();

  const char *menu_principale[] = {
    "Mostra ruleset (nft list ruleset)",
    "Mostra servizi in ascolto (ss -tulpn)",
    "Gestisci blacklist4",
    "Gestisci whitelist4",
    "Controlla configurazione (/etc/nftables.conf)",
    "Applica configurazione (/etc/nftables.conf)",
    "Mostra log kernel (journalctl -k)",
    "Esci"
  };

  while (1) {
    clear();
    draw_topbar();

    int sel = menu_centrato("Menu principale", menu_principale, 8);
    if (sel < 0 || sel == 7) break;

    if (sel == 0) {
      int code=0; char *out = run_cmd_capture("nft list ruleset", &code);
      pager_show(code==0 ? "Ruleset" : "Ruleset (errore)", out);
      free(out);
    } else if (sel == 1) {
      char *out = run_cmd_capture("ss -tulpn", NULL);
      pager_show("Servizi in ascolto", out);
      free(out);
    } else if (sel == 2) {
      gestisci_set("blacklist4");
    } else if (sel == 3) {
      gestisci_set("whitelist4");
    } else if (sel == 4) {
      int code=0; char *out = run_cmd_capture("nft -c -f /etc/nftables.conf", &code);
      pager_show(code==0 ? "Configurazione OK" : "Configurazione ERRORE", out);
      free(out);
    } else if (sel == 5) {
      int code=0; char *out = run_cmd_capture("nft -f /etc/nftables.conf", &code);
      pager_show(code==0 ? "Applicazione OK" : "Applicazione ERRORE", out);
      free(out);
    } else if (sel == 6) {
      char *out = run_cmd_capture("journalctl -k --no-pager -n 300 | tail -n 300", NULL);
      pager_show("Log kernel (ultime ~300 righe)", out);
      free(out);
    }
  }

  endwin();
  return 0;
}
