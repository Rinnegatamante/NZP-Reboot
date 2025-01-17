/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "quakedef.h"
#ifdef _WIN32
#include "winquake.h"
#endif
#include "shader.h"
/*

key up events are sent even if in console mode

*/
qboolean Editor_Key(int key, int unicode);
void Key_ConsoleInsert(const char *instext);
void Key_ClearTyping (void);

unsigned char	*key_lines[CON_EDIT_LINES_MASK+1];
int		key_linepos;
int		shift_down=false;
int		key_lastpress;

int		edit_line=0;
int		history_line=0;

unsigned int key_dest_mask;
qboolean key_dest_console;
unsigned int key_dest_absolutemouse;

struct key_cursor_s key_customcursor[kc_max];

int		key_count;			// incremented every key event

int		key_bindmaps[2];
char	*keybindings[K_MAX][KEY_MODIFIERSTATES];
qbyte	bindcmdlevel[K_MAX][KEY_MODIFIERSTATES];
qboolean	consolekeys[K_MAX];	// if true, can't be rebound while in console
qboolean	menubound[K_MAX];	// if true, can't be rebound while in menu
int		keyshift[K_MAX];		// key to map to if shift held down in console
int		key_repeats[K_MAX];	// if > 1, it is autorepeating
qboolean	keydown[K_MAX];

#define MAX_INDEVS 8

char *releasecommand[K_MAX][MAX_INDEVS];	//this is the console command to be invoked when the key is released. should free it.
qbyte releasecommandlevel[K_MAX][MAX_INDEVS];	//and this is the cbuf level it is to be run at.

static void QDECL Con_Selectioncolour_Callback(struct cvar_s *var, char *oldvalue);

extern cvar_t con_displaypossibilities;
cvar_t con_selectioncolour = CVARFC("con_selectioncolour", "0", CVAR_RENDERERCALLBACK, Con_Selectioncolour_Callback);
cvar_t con_echochat = CVAR("con_echochat", "0");
extern cvar_t cl_chatmode;

static int KeyModifier (qboolean shift, qboolean alt, qboolean ctrl)
{
	int stateset = 0;
	if (shift)
		stateset |= 1;
	if (alt)
		stateset |= 2;
	if (ctrl)
		stateset |= 4;

	return stateset;
}

void Key_GetBindMap(int *bindmaps)
{
	int i;
	for (i = 0; i < countof(key_bindmaps); i++)
	{
		if (key_bindmaps[i])
			bindmaps[i] = (key_bindmaps[i]&~KEY_MODIFIER_ALTBINDMAP) + 1;
		else
			bindmaps[i] = 0;
	}
}

void Key_SetBindMap(int *bindmaps)
{
	int i;
	for (i = 0; i < countof(key_bindmaps); i++)
	{
		if (bindmaps[i] > 0 && bindmaps[i] <= KEY_MODIFIER_ALTBINDMAP)
			key_bindmaps[i] = (bindmaps[i]-1)|KEY_MODIFIER_ALTBINDMAP;
		else
			key_bindmaps[i] = 0;
	}
}

typedef struct
{
	char	*name;
	int		keynum;
} keyname_t;

keyname_t keynames[] =
{
	{"TAB",			K_TAB},
	{"ENTER",		K_ENTER},
	{"RETURN",		K_ENTER},
	{"ESCAPE",		K_ESCAPE},
	{"SPACE",		K_SPACE},
	{"BACKSPACE",	K_BACKSPACE},
	{"UPARROW",		K_UPARROW},
	{"DOWNARROW",	K_DOWNARROW},
	{"LEFTARROW",	K_LEFTARROW},
	{"RIGHTARROW",	K_RIGHTARROW},

	{"LALT",	K_LALT},
	{"RALT",	K_RALT},
	{"LCTRL",	K_LCTRL},
	{"RCTRL",	K_RCTRL},
	{"LSHIFT",	K_LSHIFT},
	{"RSHIFT",	K_RSHIFT},
	{"ALT",		K_ALT},	//depricated name
	{"CTRL",	K_CTRL},	//depricated name
	{"SHIFT",	K_SHIFT},	//depricated name
	
	{"F1",		K_F1},
	{"F2",		K_F2},
	{"F3",		K_F3},
	{"F4",		K_F4},
	{"F5",		K_F5},
	{"F6",		K_F6},
	{"F7",		K_F7},
	{"F8",		K_F8},
	{"F9",		K_F9},
	{"F10",		K_F10},
	{"F11",		K_F11},
	{"F12",		K_F12},

	{"INS",		K_INS},
	{"DEL",		K_DEL},
	{"PGDN",	K_PGDN},
	{"PGUP",	K_PGUP},
	{"HOME",	K_HOME},
	{"END",		K_END},

	
	{"KP_HOME",		K_KP_HOME},
	{"KP_UPARROW",	K_KP_UPARROW},
	{"KP_PGUP",		K_KP_PGUP},
	{"KP_LEFTARROW", K_KP_LEFTARROW},
	{"KP_5",		K_KP_5},
	{"KP_RIGHTARROW", K_KP_RIGHTARROW},
	{"KP_END",		K_KP_END},
	{"KP_DOWNARROW",	K_KP_DOWNARROW},
	{"KP_PGDN",		K_KP_PGDN},
	{"KP_ENTER",	K_KP_ENTER},
	{"KP_INS",		K_KP_INS},
	{"KP_DEL",		K_KP_DEL},
	{"KP_SLASH",	K_KP_SLASH},
	{"KP_MINUS",	K_KP_MINUS},
	{"KP_PLUS",		K_KP_PLUS},
	{"KP_NUMLOCK",	K_KP_NUMLOCK},
	{"KP_STAR",		K_KP_STAR},
	{"KP_MULTIPLY",	K_KP_STAR},
	{"KP_EQUALS",	K_KP_EQUALS},

	//fuhquake compatible.
	{"KP_0",		K_KP_INS},
	{"KP_1",		K_KP_END},
	{"KP_2",		K_KP_DOWNARROW},
	{"KP_3",		K_KP_PGDN},
	{"KP_4",		K_KP_LEFTARROW},
	{"KP_6",		K_KP_RIGHTARROW},
	{"KP_7",		K_KP_HOME},
	{"KP_8",		K_KP_UPARROW},
	{"KP_9",		K_KP_PGUP},

	{"MOUSE1",	K_MOUSE1},
	{"MOUSE2",	K_MOUSE2},
	{"MOUSE3",	K_MOUSE3},
	{"MOUSE4",	K_MOUSE4},
	{"MOUSE5",	K_MOUSE5},
	{"MOUSE6",	K_MOUSE6},
	{"MOUSE7",	K_MOUSE7},
	{"MOUSE8",	K_MOUSE8},
	{"MOUSE9",	K_MOUSE9},
	{"MOUSE10",	K_MOUSE10},

	{"LWIN",	K_LWIN},
	{"RWIN",	K_RWIN},
	{"APP",		K_APP},
	{"MENU",	K_APP},
	{"SEARCH",	K_SEARCH},
	{"POWER",	K_POWER},
	{"VOLUP",	K_VOLUP},
	{"VOLDOWN",	K_VOLDOWN},
 
	{"JOY1",	K_JOY1},
	{"JOY2",	K_JOY2},
	{"JOY3",	K_JOY3},
	{"JOY4",	K_JOY4},

	{"AUX1",	K_AUX1},
	{"AUX2",	K_AUX2},
	{"AUX3",	K_AUX3},
	{"AUX4",	K_AUX4},
	{"AUX5",	K_AUX5},
	{"AUX6",	K_AUX6},
	{"AUX7",	K_AUX7},
	{"AUX8",	K_AUX8},
	{"AUX9",	K_AUX9},
	{"AUX10",	K_AUX10},
	{"AUX11",	K_AUX11},
	{"AUX12",	K_AUX12},
	{"AUX13",	K_AUX13},
	{"AUX14",	K_AUX14},
	{"AUX15",	K_AUX15},
	{"AUX16",	K_AUX16},
	{"AUX17",	K_AUX17},
	{"AUX18",	K_AUX18},
	{"AUX19",	K_AUX19},
	{"AUX20",	K_AUX20},
	{"AUX21",	K_AUX21},
	{"AUX22",	K_AUX22},
	{"AUX23",	K_AUX23},
	{"AUX24",	K_AUX24},
	{"AUX25",	K_AUX25},
	{"AUX26",	K_AUX26},
	{"AUX27",	K_AUX27},
	{"AUX28",	K_AUX28},
	{"AUX29",	K_AUX29},
	{"AUX30",	K_AUX30},
	{"AUX31",	K_AUX31},
	{"AUX32",	K_AUX32},

	{"PAUSE",		K_PAUSE},

	{"MWHEELUP",	K_MWHEELUP},
	{"MWHEELDOWN",	K_MWHEELDOWN},

	{"PRINTSCREEN",	K_PRINTSCREEN},
	{"CAPSLOCK",	K_CAPSLOCK},
	{"SCROLLLOCK",	K_SCRLCK},

	{"SEMICOLON",	';'},	// because a raw semicolon seperates commands
	{"PLUS",		'+'},	// because "shift++" is inferior to shift+plus

	{"TILDE",		'~'},
	{"BACKQUOTE",	'`'},
	{"BACKSLASH",	'\\'},

	{"GP_A",			K_GP_A},
	{"GP_B",			K_GP_B},
	{"GP_X",			K_GP_X},
	{"GP_Y",			K_GP_Y},
	{"GP_LSHOULDER",	K_GP_LEFT_SHOULDER},
	{"GP_RSHOULDER",	K_GP_RIGHT_SHOULDER},
	{"GP_LTRIGGER",		K_GP_LEFT_TRIGGER},
	{"GP_RTRIGGER",		K_GP_RIGHT_TRIGGER},
	{"GP_BACK",			K_GP_BACK},
	{"GP_START",		K_GP_START},
	{"GP_LTHUMB",		K_GP_LEFT_THUMB},
	{"GP_RTHUMB",		K_GP_RIGHT_THUMB},
	{"GP_DPAD_UP",		K_GP_DPAD_UP},
	{"GP_DPAD_DOWN",	K_GP_DPAD_DOWN},
	{"GP_DPAD_LEFT",	K_GP_DPAD_LEFT},
	{"GP_DPAD_RIGHT",	K_GP_DPAD_RIGHT},
	{"GP_GUIDE",		K_GP_GUIDE},
	{"GP_UNKNOWN",		K_GP_UNKNOWN},

	{NULL,			0}
};

#if defined(CSQC_DAT) || defined(MENU_DAT)
int MP_TranslateFTEtoQCCodes(int code);
void Key_PrintQCDefines(vfsfile_t *f)
{
	int i, j;
	for (i = 0; keynames[i].name; i++)
	{
		for (j = 0; j < i; j++)
			if (keynames[j].keynum == keynames[i].keynum)
				break;
		if (j == i)
			VFS_PRINTF(f, "#define K_%s\t%i\n", keynames[i].name, MP_TranslateFTEtoQCCodes(keynames[j].keynum));
	}
}
#endif

/*
==============================================================================

			LINE TYPING INTO THE CONSOLE

==============================================================================
*/

qboolean Cmd_IsCommand (char *line)
{
	char	command[128];
	char	*cmd, *s;
	int		i;

	s = line;

	for (i=0 ; i<127 ; i++)
		if (s[i] <= ' ' || s[i] == ';')
			break;
		else
			command[i] = s[i];
	command[i] = 0;

	cmd = Cmd_CompleteCommand (command, true, false, -1, NULL);
	if (!cmd  || strcmp (cmd, command) )
		return false;		// just a chat message
	return true;
}

#define COLUMNWIDTH 20
#define MINCOLUMNWIDTH 18

int PaddedPrint (char *s, int x)
{
	Con_Printf ("^4%s\t", s);
	x+=strlen(s);

	return x;
}

int con_commandmatch;
void CompleteCommand (qboolean force)
{
	char	*cmd, *s;
	const char *desc;

	s = key_lines[edit_line];
	if (*s == '\\' || *s == '/')
		s++;

	for (cmd = s; *cmd; cmd++)
	{
		if (*cmd == ' ' || *cmd == '\t')
			break;
	}
	if (*cmd)
		cmd = s;
	else
	{
		//check for singular matches and complete if found
		cmd = force?NULL:Cmd_CompleteCommand (s, true, true, 2, NULL);
		if (!cmd)
		{
			if (!force)
				cmd = Cmd_CompleteCommand (s, false, true, 1, &desc);
			else
				cmd = Cmd_CompleteCommand (s, true, true, con_commandmatch, &desc);
			if (cmd)
			{
				//complete to that (maybe partial) cmd.
				Key_ClearTyping();
				Key_ConsoleInsert("/");
				Key_ConsoleInsert(cmd);
				s = key_lines[edit_line]+1;

				//if its the only match, add a space ready for arguments.
				cmd = Cmd_CompleteCommand (s, true, true, 0, NULL);
				if (cmd && !strcmp(s, cmd))
				{
					Key_ConsoleInsert(" ");
				}

				if (!con_commandmatch)
					con_commandmatch = 1;

				if (desc)
					Con_Footerf(NULL, false, "%s: %s", cmd, desc);
				else
					Con_Footerf(NULL, false, "");
				return;
			}
		}
		//complete to a partial match.
		cmd = Cmd_CompleteCommand (s, false, true, 0, &desc);
		if (cmd)
		{
			int i = key_lines[edit_line][0] == '/'?1:0;
			if (i != 1 || strcmp(key_lines[edit_line]+i, cmd))
			{	//if successful, use that instead.
				Key_ClearTyping();
				Key_ConsoleInsert("/");
				Key_ConsoleInsert(cmd);

				s = key_lines[edit_line];	//readjust to cope with the insertion of a /
				if (*s == '\\' || *s == '/')
					s++;
			}
		}
	}
	con_commandmatch++;
	cmd = Cmd_CompleteCommand(s, true, true, con_commandmatch, &desc);
	if (!cmd)
	{
		con_commandmatch = 1;
		cmd = Cmd_CompleteCommand(s, true, true, con_commandmatch, &desc);
	}
	if (cmd)
	{
		cvar_t *var = Cvar_FindVar(cmd);
		if (var)
		{
			if (desc)
				Con_Footerf(NULL, false, "%s %s\n%s", cmd, var->string, desc);
			else
				Con_Footerf(NULL, false, "%s %s", cmd, var->string);
		}
		else
		{
			if (desc)
				Con_Footerf(NULL, false, "%s: %s", cmd, desc);
			else
				Con_Footerf(NULL, false, "");
		}
	}
	else
	{
		Con_Footerf(NULL, false, "");
		con_commandmatch = 1;
	}
}

int Con_Navigate(console_t *con, char *line)
{
	if (con->backshader)
	{
#ifdef HAVE_MEDIA_DECODER
		cin_t *cin = R_ShaderGetCinematic(con->backshader);
		if (cin)
		{
			Media_Send_Command(cin, line);
		}
#endif
	}
	con->linebuffered = NULL;
	return 2;
}

//lines typed at the main console enter here
int Con_ExecuteLine(console_t *con, char *line)
{
	qboolean waschat = false;
	char *deutf8 = NULL;
	if (com_parseutf8.ival <= 0)
	{
		unsigned int unicode;
		int err;
		int len = 0;
		int maxlen = strlen(line)*6+1;
		deutf8 = malloc(maxlen);
		while(*line)
		{
			unicode = utf8_decode(&err, line, &line);
			len += unicode_encode(deutf8+len, unicode, maxlen-1 - len, true);
		}
		deutf8[len] = 0;
		line = deutf8;
	}

	con_commandmatch=1;
	Con_Footerf(con, false, "");

	if (cls.state >= ca_connected && cl_chatmode.value == 2)
	{
		waschat = true;
		if (keydown[K_CTRL])
			Cbuf_AddText ("say_team ", RESTRICT_LOCAL);
		else if (keydown[K_SHIFT] || *line == ' ')
			Cbuf_AddText ("say ", RESTRICT_LOCAL);
		else
			waschat = false;
	}
	while (*line == ' ')
		line++;
	if (waschat)
		Cbuf_AddText (line, RESTRICT_LOCAL);
	else
	{
		char *exec = NULL;
		if (line[0] == '\\' || line[0] == '/')
			exec = line+1;	// skip the slash
		else if (cl_chatmode.value == 2 && Cmd_IsCommand(line))
			exec = line;	// valid command
	#ifdef Q2CLIENT
		else if (cls.protocol == CP_QUAKE2)
			exec = line;	// send the command to the server via console, and let the server convert to chat
	#endif
		else if (*line)
		{	// convert to a chat message
			if ((cl_chatmode.value == 1 || ((cls.state >= ca_connected && cl_chatmode.value == 2) && (strncmp(line, "say ", 4)))))
			{
				if (keydown[K_CTRL])
					Cbuf_AddText ("say_team ", RESTRICT_LOCAL);
				else
					Cbuf_AddText ("say ", RESTRICT_LOCAL);
				waschat = true;
				Cbuf_AddText (line, RESTRICT_LOCAL);
			}
		}

		if (exec)
		{
#ifdef TEXTEDITOR
			if (editormodal)
			{
				char cvarname[128];
				COM_ParseOut(exec, cvarname, sizeof(cvarname));
				if (Cvar_FindVar(cvarname) && !strchr(line, ';') && !strchr(line, '\n'))
				{
					Con_Printf ("]%s\n",line);
					Cmd_ExecuteString(exec, RESTRICT_SERVER);
					free(deutf8);
					return true;
				}

				Con_Footerf(con, false, "Commands cannot be execed while debugging QC");
			}
#endif
			Cbuf_AddText (exec, RESTRICT_LOCAL);
		}
	}

	Cbuf_AddText ("\n", RESTRICT_LOCAL);
	if (!waschat || con_echochat.value)
	{
		Con_Printf ("%s", con->prompt);
		Con_Printf ("%s\n",line);
	}

//	if (cls.state == ca_disconnected)
//		SCR_UpdateScreen ();	// force an update, because the command
//									// may take some time

	free(deutf8);
	return true;
}

vec3_t sccolor;

static void QDECL Con_Selectioncolour_Callback(struct cvar_s *var, char *oldvalue)
{
	if (qrenderer != QR_NONE)
		SCR_StringToRGB(var->string, sccolor, 1);
}

qboolean Key_GetConsoleSelectionBox(console_t *con, int *sx, int *sy, int *ex, int *ey)
{
	*sx = *sy = *ex = *ey = 0;

	if (con->buttonsdown == CB_SCROLL)
	{
		//left-mouse.
		//scroll the console with the mouse. trigger links on release.
		while (con->mousecursor[1] - con->mousedown[1] > 8 && con->display->older)
		{
			con->mousedown[1] += 8;
			con->display = con->display->older;
		}
		while (con->mousecursor[1] - con->mousedown[1] < -8 && con->display->newer)
		{
			con->mousedown[1] -= 8;
			con->display = con->display->newer;
		}

		*sx = con->mousecursor[0];
		*sy = con->mousecursor[1];
		*ex = con->mousecursor[0];
		*ey = con->mousecursor[1];
		return true;
	}
	else if (con->buttonsdown == CB_COPY || con->buttonsdown == CB_SELECT)
	{
		//right-mouse
		//select. copy-to-clipboard on release.
		*sx = con->mousedown[0];
		*sy = con->mousedown[1];
		*ex = con->mousecursor[0];
		*ey = con->mousecursor[1];
		return true;
	}
	else
	{
		if (con_curwindow == con && con->buttonsdown)
		{
			if (con->buttonsdown == CB_MOVE)
			{	//move window to track the cursor
				con->wnd_x += con->mousecursor[0] - con->mousedown[0];
		//		con->mousedown[0] = con->mousecursor[0];
				con->wnd_y += con->mousecursor[1] - con->mousedown[1];
		//		con->mousedown[1] = con->mousecursor[1];
			}
			if (con->buttonsdown & CB_SIZELEFT)
			{
				if (con->wnd_w - (con->mousecursor[0] - con->mousedown[0]) >= 64)
				{
					con->wnd_w -= con->mousecursor[0] - con->mousedown[0];
					con->wnd_x += con->mousecursor[0] - con->mousedown[0];
				}
			}
			if (con->buttonsdown & CB_SIZERIGHT)
			{
				if (con->wnd_w + (con->mousecursor[0] - con->mousedown[0]) >= 64)
				{
					con->wnd_w += con->mousecursor[0] - con->mousedown[0];
					con->mousedown[0] = con->mousecursor[0];
				}
			}
			if (con->buttonsdown & CB_SIZEBOTTOM)
			{
				if (con->wnd_h + (con->mousecursor[1] - con->mousedown[1]) >= 64)
				{
					con->wnd_h += con->mousecursor[1] - con->mousedown[1];
					con->mousedown[1] = con->mousecursor[1];
				}
			}
		}
		else
			con->buttonsdown = CB_NONE;

		*sx = con->mousecursor[0];
		*sy = con->mousecursor[1];
		*ex = con->mousecursor[0];
		*ey = con->mousecursor[1];
		return false;
	}
}

/*insert the given text at the console input line at the current cursor pos*/
void Key_ConsoleInsert(const char *instext)
{
	int i;
	int len, olen;
	char *old;
	if (!*instext)
		return;

	old = key_lines[edit_line];
	len = strlen(instext);
	olen = strlen(old);
	key_lines[edit_line] = BZ_Malloc(olen + len + 1);
	memcpy(key_lines[edit_line], old, key_linepos);
	memcpy(key_lines[edit_line]+key_linepos, instext, len);
	memcpy(key_lines[edit_line]+key_linepos+len, old+key_linepos, olen - key_linepos+1);
	Z_Free(old);
	for (i = key_linepos; i < key_linepos+len; i++)
	{
		if (key_lines[edit_line][i] == '\r')
			key_lines[edit_line][i] = ' ';
		else if (key_lines[edit_line][i] == '\n')
			key_lines[edit_line][i] = ';';
	}
	key_linepos += len;
}
void Key_ConsoleReplace(const char *instext)
{
	if (!*instext)
		return;

	key_linepos = 0;
	key_lines[edit_line][key_linepos] = 0;
	Key_ConsoleInsert(instext);
}

void Key_DefaultLinkClicked(console_t *con, char *text, char *info)
{
	char *c;
	/*the engine supports specific default links*/
	/*we don't support everything. a: there's no point. b: unbindall links are evil.*/
	c = Info_ValueForKey(info, "player");
	if (*c)
	{
		unsigned int player = atoi(c);
		int i;
		if (player >= cl.allocated_client_slots || !*cl.players[player].name)
			return;

		c = Info_ValueForKey(info, "action");
		if (*c)
		{
			if (!strcmp(c, "mute"))
			{
				if (!cl.players[player].vignored)
				{
					cl.players[player].vignored = true;
					Con_Printf("^[%s\\player\\%i^] muted\n", cl.players[player].name, player);
				}
				else
				{
					cl.players[player].vignored = false;
					Con_Printf("^[%s\\player\\%i^] unmuted\n", cl.players[player].name, player);
				}
			}
			else if (!strcmp(c, "ignore"))
			{
				if (!cl.players[player].ignored)
				{
					cl.players[player].ignored = true;
					cl.players[player].vignored = true;
					Con_Printf("^[%s\\player\\%i^] ignored\n", cl.players[player].name, player);
				}
				else
				{
					cl.players[player].ignored = false;
					cl.players[player].vignored = false;
					Con_Printf("^[%s\\player\\%i^] unignored\n", cl.players[player].name, player);
				}
			}
			else if (!strcmp(c, "spec"))
			{
				Cam_TrackPlayer(0, "spectate", cl.players[player].name);
			}
			else if (!strcmp(c, "kick"))
			{
#ifndef CLIENTONLY
				if (sv.active)
				{
					//use the q3 command, because we can.
					Cbuf_AddText(va("\nclientkick %i\n", player), RESTRICT_LOCAL);
				}
				else
#endif
					Cbuf_AddText(va("\nrcon kick %s\n", cl.players[player].name), RESTRICT_LOCAL);
			}
			else if (!strcmp(c, "ban"))
			{
#ifndef CLIENTONLY
				if (sv.active)
				{
					//use the q3 command, because we can.
					Cbuf_AddText(va("\nbanname %s QuickBan\n", cl.players[player].name), RESTRICT_LOCAL);
				}
				else
#endif
					Cbuf_AddText(va("\nrcon banname %s QuickBan\n", cl.players[player].name), RESTRICT_LOCAL);
			}
			return;
		}

		Con_Footerf(con, false, "^m#^m ^[%s\\player\\%i^]: %if %ims", cl.players[player].name, player, cl.players[player].frags, cl.players[player].ping);

		for (i = 0; i < cl.splitclients; i++)
		{
			if (cl.playerview[i].playernum == player)
				break;
		}
		if (i == cl.splitclients)
		{
			extern cvar_t rcon_password;
			if (*cl.players[player].ip)
				Con_Footerf(con, true, "\n%s", cl.players[player].ip);

			if (cl.playerview[0].spectator || cls.demoplayback)
			{
				//we're spectating, or an mvd
				Con_Footerf(con, true, " ^[Spectate\\player\\%i\\action\\spec^]", player);
			}
			else
			{
				//we're playing.
				if (cls.protocol == CP_QUAKEWORLD && strcmp(cl.players[cl.playerview[0].playernum].team, cl.players[player].team))
					Con_Footerf(con, true, " ^[[Join Team %s]\\cmd\\setinfo team %s^]", cl.players[player].team, cl.players[player].team);
			}
			Con_Footerf(con, true, " ^[%sgnore\\player\\%i\\action\\ignore^]", cl.players[player].ignored?"Uni":"I", player);
	//		if (cl_voip_play.ival)
				Con_Footerf(con, true, " ^[%sute\\player\\%i\\action\\mute^]", cl.players[player].vignored?"Unm":"M",  player);

			if (!cls.demoplayback && (*rcon_password.string
#ifndef CLIENTONLY
				|| (sv.state && svs.clients[player].netchan.remote_address.type != NA_LOOPBACK)
#endif
				))
			{
				Con_Footerf(con, true, " ^[Kick\\player\\%i\\action\\kick^]", player);
				Con_Footerf(con, true, " ^[Ban\\player\\%i\\action\\ban^]", player);
			}
		}
		else
		{
			char cmdprefix[6];
			if (i == 0)
				*cmdprefix = 0;
			else
				snprintf(cmdprefix, sizeof(cmdprefix), "%i ", i+1);

			//hey look! its you!

			if (cl.playerview[i].spectator || cls.demoplayback)
			{
				//need join option here or something
			}
			else
			{
				Con_Footerf(con, true, " ^[Suicide\\cmd\\%skill^]", cmdprefix);
	#ifndef CLIENTONLY
				if (!sv.state)
					Con_Footerf(con, true, " ^[Disconnect\\cmd\\disconnect^]");
				if (cls.allow_cheats || (sv.state && sv.allocated_client_slots == 1))
	#else
				Con_Footerf(con, true, " ^[Disconnect\\cmd\\disconnect^]");
				if (cls.allow_cheats)
	#endif
				{
					Con_Footerf(con, true, " ^[Noclip\\cmd\\%snoclip^]", cmdprefix);
					Con_Footerf(con, true, " ^[Fly\\cmd\\%sfly^]", cmdprefix);
					Con_Footerf(con, true, " ^[God\\cmd\\%sgod^]", cmdprefix);
					Con_Footerf(con, true, " ^[Give\\impulse\\9^]");
				}
			}
		}
		return;
	}
	c = Info_ValueForKey(info, "menu");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nmenu_cmd conlink %s\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "connect");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nconnect \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "join");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\njoin \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	/*c = Info_ValueForKey(info, "url");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nplayfilm %s\n", c), RESTRICT_LOCAL);
		return;
	}*/
	c = Info_ValueForKey(info, "observe");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nobserve \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "qtv");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nqtvplay \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "demo");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nplaydemo \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "map");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nmap \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "modelviewer");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nmodelviewer \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "type");
	if (*c)
	{
		Key_ConsoleReplace(c);
		return;
	}
	c = Info_ValueForKey(info, "cmd");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\ncmd %s\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "dir");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\necho Contents of %s:\ndir \"%s\"\n", c, c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "edit");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nedit \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "impulse");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nimpulse %s\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "film");
	if (*c && !strchr(c, ';') && !strchr(c, '\n'))
	{
		Cbuf_AddText(va("\nplayfilm \"%s\"\n", c), RESTRICT_LOCAL);
		return;
	}
	c = Info_ValueForKey(info, "desc");
	if (*c)
	{
		Con_Footerf(con, false, "%s", c);
		return;
	}

	//if there's no info and the text starts with a leading / then insert it as a suggested/completed console command
	//skip any leading colour code.
	if (text[0] == '^' && text[1] >= '0' && text[1] <= '9')
		text+=2;
	if (*text == '/')
	{
		int tlen = info - text;
		Z_Free(key_lines[edit_line]);
		key_lines[edit_line] = BZ_Malloc(tlen + 1);
		memcpy(key_lines[edit_line], text, tlen);
		key_lines[edit_line][tlen] = 0;
		key_linepos = strlen(key_lines[edit_line]);
		return;
	}
}

void Key_ConsoleRelease(console_t *con, int key, int unicode)
{
	char *buffer;	

	if (key == K_MOUSE1 && con->buttonsdown == CB_SELECT)
	{
		if (con->selstartline)
		{
			if (con->selstartline == con->selendline && con->selendoffset <= con->selstartoffset+1)
				con->flags &= ~CONF_KEEPSELECTION;
			else
				con->flags |= CONF_KEEPSELECTION;
			con->userline = con->selstartline;
			con->useroffset = con->selstartoffset;
		}
		con->buttonsdown = CB_NONE;
	}
	if (key == K_MOUSE1 && con->buttonsdown == CB_SCROLL)
	{
		con->buttonsdown = CB_NONE;
		if (abs(con->mousedown[0] - con->mousecursor[0]) < 5 && abs(con->mousedown[1] - con->mousecursor[1]) < 5)
		{
			buffer = Con_CopyConsole(con, false, false);
			Con_Footerf(con, false, "");
			if (!buffer)
				return;
			if (keydown[K_SHIFT])
			{
				int len;
				len = strlen(buffer);
				//strip any trailing dots/elipsis
				while (len > 1 && !strcmp(buffer+len-1, "."))
				{
					len-=1;
					buffer[len] = 0;
				}
				//strip any enclosing quotes
				while (*buffer == '\"' && len > 2 && !strcmp(buffer+len-1, "\""))
				{
					len-=2;
					memmove(buffer, buffer+1, len);
					buffer[len] = 0;
				}
				Key_ConsoleInsert(buffer);
			}
			else
			{
				if (buffer[0] == '^' && buffer[1] == '[')
				{
					//looks like it might be a link!
					char *end = NULL;
					char *info;
					for (info = buffer + 2; *info; )
					{
						if (info[0] == '^' && info[1] == ']')
							break; //end of tag, with no actual info, apparently
						if (*info == '\\')
							break;
						else if (info[0] == '^' && info[1] == '^')
							info+=2;
						else
							info++;
					}
					for(end = info; *end; )
					{
						if (end[0] == '^' && end[1] == ']')
						{
							//okay, its a valid link that they clicked
							*end = 0;
#ifdef PLUGINS
							if (!Plug_ConsoleLink(buffer+2, info, con->name))
#endif
#ifdef CSQC_DAT
							if (!CSQC_ConsoleLink(buffer+2, info))
#endif
							{
								Key_DefaultLinkClicked(con, buffer+2, info);
							}

							break;
						}
						if (end[0] == '^' && end[1] == '^')
							end+=2;
						else
							end++;
					}
				}
			}
			Z_Free(buffer);
		}
		else
			Con_Footerf(con, false, "");
	}
	if (key == K_MOUSE2 && con->buttonsdown == CB_COPY)
	{
		con->buttonsdown = CB_NONE;
		buffer = Con_CopyConsole(con, true, false);	//don't keep markup if we're copying to the clipboard
		if (!buffer)
			return;
		Sys_SaveClipboard(buffer);
		Z_Free(buffer);
	}
	if (con->buttonsdown == CB_CLOSE)
	{	//window X (close)
		if (con->mousecursor[0] > con->wnd_w-16 && con->mousecursor[1] < 8)
		{
			if (con->close && !con->close(con, false))
				return;
			Con_Destroy (con);
			return;
		}
	}
//	if (con->buttonsdown == CB_MOVE)	//window title(move)
		con->buttonsdown = CB_NONE;

#ifdef HAVE_MEDIA_DECODER
	if (con->backshader)
	{
		cin_t *cin = R_ShaderGetCinematic(con->backshader);
		if (cin)
			Media_Send_KeyEvent(cin, key, unicode, 1);
	}
#endif
}
//if the referenced (trailing) chevron is doubled up, then it doesn't act as part of any markup and should be ignored for such things.
static qboolean utf_specialchevron(unsigned char *start, unsigned char *chev)
{
	int count = 0;
	while (chev >= start)
	{
		if (*chev-- == '^')
			count++;
		else
			break;
	}
	return count&1;
}
//move the cursor one char to the left. cursor must be within the 'start' string.
static unsigned char *utf_left(unsigned char *start, unsigned char *cursor)
{
	if (cursor == start)
		return cursor;
	if (1)//com_parseutf8.ival>0)
	{
		cursor--;
		while ((*cursor & 0xc0) == 0x80 && cursor > start)
			cursor--;
	}
	else
		cursor--;

	//FIXME: should verify that the ^ isn't doubled.
	if (*cursor == ']' && cursor > start && utf_specialchevron(start, cursor-1))
	{
		//just stepped onto a link
		unsigned char *linkstart;
		linkstart = cursor-1;
		while(linkstart >= start)
		{
			//FIXME: should verify that the ^ isn't doubled.
			if (utf_specialchevron(start, linkstart) && linkstart[1] == '[')
				return linkstart;
			linkstart--;
		}
	}

	return cursor;
}

//move the cursor one char to the right.
static unsigned char *utf_right(unsigned char *start, unsigned char *cursor)
{
	//FIXME: should make sure this is not doubled.
	if (utf_specialchevron(start, cursor) && cursor[1] == '[')
	{
		//just stepped over a link
		char *linkend;
		linkend = cursor+2;
		while(*linkend)
		{
			if (utf_specialchevron(start, linkend) && linkend[1] == ']')
				return linkend+2;
			else
				linkend++;
		}
		return linkend;
	}

	if (1)//com_parseutf8.ival>0)
	{
		int skip = 1;
		//figure out the length of the char
		if ((*cursor & 0xc0) == 0x80)
			skip = 1;	//error
		else if ((*cursor & 0xe0) == 0xc0)
			skip = 2;
		else if ((*cursor & 0xf0) == 0xe0)
			skip = 3;
		else if ((*cursor & 0xf1) == 0xf0)
			skip = 4;
		else if ((*cursor & 0xf3) == 0xf1)
			skip = 5;
		else if ((*cursor & 0xf7) == 0xf3)
			skip = 6;
		else if ((*cursor & 0xff) == 0xf7)
			skip = 7;
		else skip = 1;

		while (*cursor && skip)
		{
			cursor++;
			skip--;
		}
	}
	else if (*cursor)
		cursor++;

	return cursor;
}

void Key_EntryInsert(unsigned char **line, int *linepos, char *instext)
{
	int i;
	int len, olen;
	char *old;

	if (!*instext)
		return;

	old = (*line);
	len = strlen(instext);
	olen = strlen(old);
	*line = BZ_Malloc(olen + len + 1);
	memcpy(*line, old, *linepos);
	memcpy(*line+*linepos, instext, len);
	memcpy(*line+*linepos+len, old+*linepos, olen - *linepos+1);
	Z_Free(old);
	for (i = *linepos; i < *linepos+len; i++)
	{
		if ((*line)[i] == '\r')
			(*line)[i] = ' ';
		else if ((*line)[i] == '\n')
			(*line)[i] = ';';
	}
	*linepos += len;
}

qboolean Key_EntryLine(unsigned char **line, int lineoffset, int *linepos, int key, unsigned int unicode)
{
	qboolean ctrl = keydown[K_LCTRL] || keydown[K_RCTRL];
	qboolean shift = keydown[K_LSHIFT] || keydown[K_RSHIFT];
	char utf8[8];

	if (key == K_LEFTARROW || key == K_KP_LEFTARROW)
	{
		if (ctrl)
		{
			//ignore whitespace if we're at the end of the word
			while (*linepos > 0 && (*line)[*linepos-1] == ' ')
				*linepos = utf_left((*line)+lineoffset, (*line) + *linepos) - (*line);
			//keep skipping until we find the start of that word
			while (ctrl && *linepos > lineoffset && (*line)[*linepos-1] != ' ')
				*linepos = utf_left((*line)+lineoffset, (*line) + *linepos) - (*line);
		}
		else
			*linepos = utf_left((*line)+lineoffset, (*line) + *linepos) - (*line);
		return true;
	}
	if (key == K_RIGHTARROW || key == K_KP_RIGHTARROW)
	{
		if ((*line)[*linepos])
		{
			*linepos = utf_right((*line)+lineoffset, (*line) + *linepos) - (*line);
			if (ctrl)
			{
				//skip over the word
				while ((*line)[*linepos] && (*line)[*linepos] != ' ')
					*linepos = utf_right((*line)+lineoffset, (*line) + *linepos) - (*line);
				//as well as any trailing whitespace
				while ((*line)[*linepos] == ' ')
					*linepos = utf_right((*line)+lineoffset, (*line) + *linepos) - (*line);
			}
			return true;
		}
		else
			unicode = ' ';
	}

	if (key == K_DEL || key == K_KP_DEL)
	{
		if ((*line)[*linepos])
		{
			int charlen = utf_right((*line)+lineoffset, (*line) + *linepos) - ((*line) + *linepos);
			memmove((*line)+*linepos, (*line)+*linepos+charlen, strlen((*line)+*linepos+charlen)+1);
			return true;
		}
		else
			key = K_BACKSPACE;
	}

	if (key == K_BACKSPACE)
	{
		if (*linepos > lineoffset)
		{
			int charlen = ((*line)+*linepos) - utf_left((*line)+lineoffset, (*line) + *linepos);
			memmove((*line)+*linepos-charlen, (*line)+*linepos, strlen((*line)+*linepos)+1);
			*linepos -= charlen;
		}
		if (!(*line)[lineoffset])	//oops?
			con_commandmatch = 0;
		return true;
	}



	if (key == K_HOME || key == K_KP_HOME)
	{
		*linepos = lineoffset;
		return true;
	}

	if (key == K_END || key == K_KP_END)
	{
		*linepos = strlen(*line);
		return true;
	}

	//beware that windows translates ctrl+c and ctrl+v to a control char
	if (((unicode=='C' || unicode=='c' || unicode==3) && ctrl) || (ctrl && key == K_INS))
	{
		Sys_SaveClipboard(*line);
		return true;
	}

	if (((unicode=='V' || unicode=='v' || unicode==22) && ctrl) || (shift && key == K_INS))
	{
		char *clipText = Sys_GetClipboard();
		if (clipText)
		{
			Key_EntryInsert(line, linepos, clipText);
			Sys_CloseClipboard(clipText);
		}
		return true;
	}

	if (unicode < ' ')
	{
		//if the user is entering control codes, then the ctrl+foo mechanism is probably unsupported by the unicode input stuff, so give best-effort replacements.
		switch(unicode)
		{
		case 27/*'['*/: unicode = 0xe010; break;
		case 29/*']'*/: unicode = 0xe011; break;
		case 7/*'g'*/: unicode = 0xe086; break;
		case 18/*'r'*/: unicode = 0xe087; break;
		case 25/*'y'*/: unicode = 0xe088; break;
		case 2/*'b'*/: unicode = 0xe089; break;
		case 19/*'s'*/: unicode = 0xe080; break;
		case 4/*'d'*/: unicode = 0xe081; break;
		case 6/*'f'*/: unicode = 0xe082; break;
		case 1/*'a'*/: unicode = 0xe083; break;
		case 21/*'u'*/: unicode = 0xe01d; break;
		case 9/*'i'*/: unicode = 0xe01e; break;
		case 15/*'o'*/: unicode = 0xe01f; break;
		case 10/*'j'*/: unicode = 0xe01c; break;
		case 16/*'p'*/: unicode = 0xe09c; break;
		case 13/*'m'*/: unicode = 0xe08b; break;
		case 11/*'k'*/: unicode = 0xe08d; break;
		case 14/*'n'*/: unicode = '\r'; break;
		default:
//			if (unicode)
//				Con_Printf("escape code %i\n", unicode);

			//even if we don't print these, we still need to cancel them in the caller.
			if (key == K_LALT || key == K_RALT ||
				key == K_LCTRL || key == K_RCTRL ||
				key == K_LSHIFT || key == K_RSHIFT)
				return true;
			return false;
		}
	}
#ifndef FTE_TARGET_WEB	//browser port gets keys stuck down when task switching, especially alt+tab. don't confuse users.
	else if (com_parseutf8.ival >= 0)	//don't do this for iso8859-1. the major user of that is hexen2 which doesn't have these chars.
	{
		if (ctrl && !keydown[K_RALT])
		{
			if (unicode >= '0' && unicode <= '9')
				unicode = unicode - '0' + 0xe012;	// yellow number
			else switch (unicode)
			{
				case '[': unicode = 0xe010; break;
				case ']': unicode = 0xe011; break;
				case 'g': unicode = 0xe086; break;
				case 'r': unicode = 0xe087; break;
				case 'y': unicode = 0xe088; break;
				case 'b': unicode = 0xe089; break;
				case '(': unicode = 0xe080; break;
				case '=': unicode = 0xe081; break;
				case ')': unicode = 0xe082; break;
				case 'a': unicode = 0xe083; break;
				case '<': unicode = 0xe01d; break;
				case '-': unicode = 0xe01e; break;
				case '>': unicode = 0xe01f; break;
				case ',': unicode = 0xe01c; break;
				case '.': unicode = 0xe09c; break;
				case 'B': unicode = 0xe08b; break;
				case 'C': unicode = 0xe08d; break;
				case 'n': unicode = '\r'; break;
			}
		}

		if (keydown[K_LALT] && unicode > 32 && unicode < 128)
			unicode |= 0xe080;		// red char
	}
#endif

	unicode = utf8_encode(utf8, unicode, sizeof(utf8)-1);
	if (unicode)
	{
		utf8[unicode] = 0;
		Key_EntryInsert(line, linepos, utf8);
		return true;
	}

	return false;
}

/*
====================
Key_Console

Interactive line editing and console scrollback
====================
*/
qboolean Key_Console (console_t *con, unsigned int unicode, int key)
{
	qboolean ctrl = keydown[K_LCTRL] || keydown[K_RCTRL];
	qboolean shift = keydown[K_LSHIFT] || keydown[K_RSHIFT];
	int rkey = key;

	//weirdness for the keypad.
	if ((unicode >= '0' && unicode <= '9') || unicode == '.')
		key = 0;

	if (con->redirect)
	{
		if (key == K_TAB)
		{	// command completion
			if (ctrl || shift)
			{
				Con_CycleConsole();
				return true;
			}
		}
		if (key == K_MOUSE1 || key == K_MOUSE2)
			;
		else if (con->redirect(con, unicode, key))
			return true;
	}

	if ((key == K_MOUSE1 || key == K_MOUSE2))
	{
		if (con->flags & CONF_ISWINDOW)
			if (con->mousecursor[0] < -8 || con->mousecursor[1] < 0 || con->mousecursor[0] > con->wnd_w || con->mousecursor[1] > con->wnd_h)
				return true;
		if (con == con_mouseover)
		{
			con->buttonsdown = CB_NONE;

			if ((con->flags & CONF_ISWINDOW) && !keydown[K_SHIFT])
				Con_SetActive(con);
		}
		con->mousedown[0] = con->mousecursor[0];
		con->mousedown[1] = con->mousecursor[1];
		if (con_mouseover && con->mousedown[1] < 8)//(8.0*vid.height)/vid.pixelheight)
		{
			if (key == K_MOUSE2 && !(con->flags & CONF_ISWINDOW))
			{
				if (con->close && !con->close(con, true))
					return true;
				Con_Destroy (con);
			}
			else
			{
				Con_SetActive(con);
				if ((con->flags & CONF_ISWINDOW))
					con->buttonsdown = (con->mousedown[0] > con->wnd_w-16)?CB_CLOSE:CB_MOVE;
			}
		}
		else if (con_mouseover && con->mousedown[1] < 16)
			con->buttonsdown = CB_ACTIONBAR;
		else if (key == K_MOUSE2)
		{
			if (con->redirect && con->redirect(con, unicode, key))
				return true;
			con->buttonsdown = CB_COPY;
			con->flags &= ~CONF_KEEPSELECTION;
		}
		else
		{
			con->buttonsdown = CB_NONE;
			if ((con->flags & CONF_ISWINDOW) && con->mousedown[0] < 0)
				con->buttonsdown |= CB_SIZELEFT;
			if ((con->flags & CONF_ISWINDOW) && con->mousedown[0] > con->wnd_w-16)
				con->buttonsdown |= CB_SIZERIGHT;
			if ((con->flags & CONF_ISWINDOW) && con->mousedown[1] > con->wnd_h-8)
				con->buttonsdown |= CB_SIZEBOTTOM;
			if (con->buttonsdown == CB_NONE)
			{
				if (con->redirect && con->redirect(con, unicode, key))
					return true;
				con->buttonsdown = CB_SCROLL;
				con->flags &= ~CONF_KEEPSELECTION;
			}
		}

		if ((con->buttonsdown == CB_COPY || con->buttonsdown == CB_SCROLL) && !con->linecount && (!con->linebuffered || con->linebuffered == Con_Navigate))
			con->buttonsdown = CB_NONE;
		else
			return true;
	}

	if (key == K_PGUP || key == K_KP_PGUP || key==K_MWHEELUP)
	{
		conline_t *l;
		int i = 2;
		if (ctrl)
			i = 8;
		if (!con->display)
			return true;
		if (con->display == con->current)
			i+=2;	//skip over the blank input line, and extra so we actually move despite the addition of the ^^^^^ line
		if (con->display->older != NULL)
		{
			while (i-->0)
			{
				if (con->display->older == NULL)
					break;
				con->display = con->display->older;
				con->display->time = realtime;
			}
			for (l = con->display; l; l = l->older)
				l->time = realtime;
			return true;
		}
	}
	if (key == K_PGDN || key == K_KP_PGDN || key==K_MWHEELDOWN)
	{
		int i = 2;
		if (ctrl)
			i = 8;
		if (!con->display)
			return true;
		if (con->display->newer != NULL)
		{
			while (i-->0)
			{
				if (con->display->newer == NULL)
					break;
				con->display = con->display->newer;
				con->display->time = realtime;
			}
			if (con->display->newer && con->display->newer == con->current)
				con->display = con->current;
			return true;
		}
	}

	if ((key == K_HOME || key == K_KP_HOME) && ctrl)
	{
		if (con->display != con->oldest)
		{
			con->display = con->oldest;
			return true;
		}
	}

	if ((key == K_END || key == K_KP_END) && ctrl)
	{
		if (con->display != con->current)
		{
			con->display = con->current;
			return true;
		}
	}

#ifdef TEXTEDITOR
	if (editormodal)
	{
		if (Editor_Key(key, unicode))
			return true;
	}
#endif

	//console does not have any way to accept input, so don't try giving it any.
	if (!con->linebuffered)
	{
#ifdef HAVE_MEDIA_DECODER
		if (con->backshader)
		{
			cin_t *cin = R_ShaderGetCinematic(con->backshader);
			if (cin)
			{
				Media_Send_KeyEvent(cin, rkey, unicode, 0);
				return true;
			}
		}
#endif
		return false;
	}
	
	if (key == K_ENTER || key == K_KP_ENTER)
	{	// backslash text are commands, else chat
		int oldl = edit_line;

#ifndef FTE_TARGET_WEB
		if (keydown[K_LALT] || keydown[K_RALT])
			Cbuf_AddText("\nvid_toggle\n", RESTRICT_LOCAL);
#endif

		if (con_commandmatch)
		{	//if that isn't actually a command, and we can actually complete it to something, then lets try to complete it.
			char *txt = key_lines[edit_line]+1;
			if (*txt == '/')
				txt++;
			if (!Cmd_IsCommand(txt) && !Cmd_CompleteCommand(txt, true, true, con_commandmatch, NULL))
			{
				CompleteCommand (true);
				return true;
			}
		}


		if (con->linebuffered)
		{
			if (con->linebuffered(con, key_lines[oldl]) != 2)
			{
				edit_line = (edit_line + 1) & (CON_EDIT_LINES_MASK);
				history_line = edit_line;
			}
		}
		con_commandmatch = 0;

		Z_Free(key_lines[edit_line]);
		key_lines[edit_line] = BZ_Malloc(1);
		key_lines[edit_line][0] = '\0';
		key_linepos = 0;
		return true;
	}

	if (key == K_SPACE && ctrl && con->commandcompletion)
	{
		char *txt = key_lines[edit_line]+1;
		if (*txt == '/')
			txt++;
		if (Cmd_CompleteCommand(txt, true, true, con->commandcompletion, NULL))
		{
			CompleteCommand (true);
			return true;
		}
	}

	if (key == K_TAB)
	{	// command completion
		if (shift)
		{
			Con_CycleConsole();
			return true;
		}

		if (con->commandcompletion)
			CompleteCommand (ctrl);
		return true;
	}
	if (key != K_CTRL && key != K_SHIFT && con_commandmatch)
		con_commandmatch=1;
	
	if (key == K_UPARROW || key == K_KP_UPARROW)
	{
		do
		{
			history_line = (history_line - 1) & CON_EDIT_LINES_MASK;
		} while (history_line != edit_line
				&& !key_lines[history_line][0]);
		if (history_line == edit_line)
			history_line = (edit_line+1)&CON_EDIT_LINES_MASK;
		key_lines[edit_line] = BZ_Realloc(key_lines[edit_line], strlen(key_lines[history_line])+1);
		Q_strcpy(key_lines[edit_line], key_lines[history_line]);
		key_linepos = Q_strlen(key_lines[edit_line]);

		if (!key_lines[edit_line][0])
			con_commandmatch = 0;
		return true;
	}

	if (key == K_DOWNARROW || key == K_KP_DOWNARROW)
	{
		if (history_line == edit_line)
		{
			key_lines[edit_line][0] = '\0';
			key_linepos=0;
			con_commandmatch = 0;
			return true;
		}
		do
		{
			history_line = (history_line + 1) & CON_EDIT_LINES_MASK;
		}
		while (history_line != edit_line
			&& !key_lines[history_line][1]);
		if (history_line == edit_line)
		{
			key_lines[edit_line][0] = '\0';
			key_linepos = 0;
		}
		else
		{
			key_lines[edit_line] = BZ_Realloc(key_lines[edit_line], strlen(key_lines[history_line])+1);
			Q_strcpy(key_lines[edit_line], key_lines[history_line]);
			key_linepos = Q_strlen(key_lines[edit_line]);
		}
		return true;
	}

	if (rkey && !consolekeys[rkey])
	{
		if (rkey != '`' || key_linepos==0)
			return false;
	}
	Key_EntryLine(&key_lines[edit_line], 0, &key_linepos, key, unicode);
	return true;
}

//============================================================================

qboolean	chat_team;
unsigned char		*chat_buffer;
int			chat_bufferpos;

void Key_Message (int key, int unicode)
{
	if (!chat_buffer)
	{
		chat_buffer = BZ_Malloc(1);
		chat_buffer[0] = 0;
		chat_bufferpos = 0;
	}

	if (key == K_ENTER || key == K_KP_ENTER)
	{
		if (chat_buffer && chat_buffer[0])
		{	//send it straight into the command.
			char *line = chat_buffer;
			char deutf8[8192];
			if (com_parseutf8.ival <= 0)
			{
				unsigned int unicode;
				int err;
				int len = 0;
				while(*line)
				{
					unicode = utf8_decode(&err, line, &line);
					len += unicode_encode(deutf8+len, unicode, sizeof(deutf8)-1 - len, true);
				}
				deutf8[len] = 0;
				line = deutf8;
			}

			Cmd_TokenizeString(va("%s %s", chat_team?"say_team":"say", line), true, false);
			CL_Say(chat_team, "");
		}

		Key_Dest_Remove(kdm_message);
		chat_bufferpos = 0;
		chat_buffer[0] = 0;
		return;
	}

	if (key == K_ESCAPE)
	{
		Key_Dest_Remove(kdm_message);
		chat_bufferpos = 0;
		chat_buffer[0] = 0;
		return;
	}

	Key_EntryLine(&chat_buffer, 0, &chat_bufferpos, key, unicode);
}

//============================================================================

//for qc
char *Key_GetBinding(int keynum, int bindmap, int modifier)
{
	char *key = NULL;
	if (keynum < 0 || keynum >= K_MAX)
		;
	else if (bindmap < 0)
	{
		key = NULL;
		if (!key)
			key = keybindings[keynum][key_bindmaps[0]];
		if (!key)
			key = keybindings[keynum][key_bindmaps[1]];
	}
	else
	{
		if (bindmap)
			modifier = (bindmap-1) + KEY_MODIFIER_ALTBINDMAP;
		if (modifier >= 0 && modifier < KEY_MODIFIERSTATES)
			key = keybindings[keynum][modifier];
	}
	return key;
}

/*
===================
Key_StringToKeynum

Returns a key number to be used to index keybindings[] by looking at
the given string.  Single ascii characters return themselves, while
the K_* names are matched up.
===================
*/
int Key_StringToKeynum (const char *str, int *modifier)
{
	keyname_t	*kn;

	if (!strnicmp(str, "std_", 4) || !strnicmp(str, "std+", 4))
		*modifier = 0;
	else
	{
		struct
		{
			char *prefix;
			int len;
			int mod;
		} mods[] =
		{
			{"shift",	5, KEY_MODIFIER_SHIFT},
			{"ctrl",	4, KEY_MODIFIER_CTRL},
			{"alt",		3, KEY_MODIFIER_ALT},
		};
		int i;
		*modifier = 0;
		for (i = 0; i < countof(mods); )
		{
			if (!Q_strncasecmp(mods[i].prefix, str, mods[i].len))
				if (str[mods[i].len] == '_' || str[mods[i].len] == '+' || str[mods[i].len] == ' ')
				if (str[mods[i].len+1])
				{
					*modifier |= mods[i].mod;
					str += mods[i].len+1;
					i = 0;
					continue;
				}
			i++;
		}
		if (!*modifier)
			*modifier = ~0;
	}
	
	if (!str || !str[0])
		return -1;
	if (!str[1])	//single char.
	{
#if 0//def _WIN32
		return VkKeyScan(str[0]);
#else
		return str[0];
#endif
	}

	if (!strncmp(str, "K_", 2))
		str+=2;

	for (kn=keynames ; kn->name ; kn++)
	{
		if (!Q_strcasecmp(str,kn->name))
			return kn->keynum;
	}
	if (atoi(str))	//assume ascii code. (prepend with a 0 if needed)
	{
		return atoi(str);
	}
	return -1;
}

/*
===================
Key_KeynumToString

Returns a string (either a single ascii char, or a K_* name) for the
given keynum.
FIXME: handle quote special (general escape sequence?)
===================
*/
static char *Key_KeynumToStringRaw (int keynum)
{
	keyname_t	*kn;	
	static	char	tinystr[2];
	
	if (keynum == -1)
		return "<KEY NOT FOUND>";
	if (keynum > 32 && keynum < 127 && keynum != '\'' && keynum != '\"')
	{	// printable ascii
		tinystr[0] = keynum;
		tinystr[1] = 0;
		return tinystr;
	}
	
	for (kn=keynames ; kn->name ; kn++)
		if (keynum == kn->keynum)
			return kn->name;

	{
		if (keynum < 10)	//don't let it be a single character
			return va("0%i", keynum);
		return va("%i", keynum);
	}

	return "<UNKNOWN KEYNUM>";
}

char *Key_KeynumToString (int keynum, int modifier)
{
	char *r = Key_KeynumToStringRaw(keynum);
	if (r[0] == '<' && r[1])
		modifier = 0;	//would be too weird.
	switch(modifier)
	{
	case KEY_MODIFIER_CTRL|KEY_MODIFIER_ALT|KEY_MODIFIER_SHIFT:
		return va("Ctrl+Alt+Shift+%s", r);
	case KEY_MODIFIER_ALT|KEY_MODIFIER_SHIFT:
		return va("Alt+Shift+%s", r);
	case KEY_MODIFIER_CTRL|KEY_MODIFIER_SHIFT:
		return va("Ctrl+Shift+%s", r);
	case KEY_MODIFIER_CTRL|KEY_MODIFIER_ALT:
		return va("Ctrl+Alt+%s", r);
	case KEY_MODIFIER_CTRL:
		return va("Ctrl+%s", r);
	case KEY_MODIFIER_ALT:
		return va("Alt+%s", r);
	case KEY_MODIFIER_SHIFT:
		return va("Shift+%s", r);
	default:
		return r;	//no modifier or a bindmap
	}
}

/*
===================
Key_SetBinding
===================
*/
void Key_SetBinding (int keynum, int modifier, char *binding, int level)
{
	char	*newc;
	int		l;

	if (modifier == ~0)	//all of the possibilities.
	{
		if (binding)
		{	//bindmaps are meant to be independant of each other.
			for (l = 0; l < KEY_MODIFIER_ALTBINDMAP; l++)
				Key_SetBinding(keynum, l, binding, level);
		}
		else
		{	//when unbinding, unbind all bindmaps.
			for (l = 0; l < KEY_MODIFIERSTATES; l++)
				Key_SetBinding(keynum, l, binding, level);
		}
		return;
	}
			
	if (keynum < 0 || keynum >= K_MAX)
		return;

	//just so the quit menu realises it needs to show something.
	Cvar_ConfigChanged();

// free old bindings
	if (keybindings[keynum][modifier])
	{
		Z_Free (keybindings[keynum][modifier]);
		keybindings[keynum][modifier] = NULL;
	}


	if (!binding)
	{
		keybindings[keynum][modifier] = NULL;
		return;
	}
// allocate memory for new binding
	l = Q_strlen (binding);	
	newc = Z_Malloc (l+1);
	Q_strcpy (newc, binding);
	newc[l] = 0;
	keybindings[keynum][modifier] = newc;
	bindcmdlevel[keynum][modifier] = level;
}

/*
===================
Key_Unbind_f
===================
*/
void Key_Unbind_f (void)
{
	int		b, modifier;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("unbind <key> : remove commands from a key\n");
		return;
	}
	
	b = Key_StringToKeynum (Cmd_Argv(1), &modifier);
	if (b==-1)
	{
		if (cl_warncmd.ival)
			Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	Key_SetBinding (b, modifier, NULL, Cmd_ExecLevel);
}

void Key_Unbindall_f (void)
{
	int		i;
	
	for (i=0 ; i<K_MAX ; i++)
		if (keybindings[i])
			Key_SetBinding (i, ~0, NULL, Cmd_ExecLevel);
}

void Key_AliasEdit_f (void)
{
	char *alias = Cmd_AliasExist(Cmd_Argv(1), RESTRICT_LOCAL);
	char quotedalias[2048];
	if (alias)
	{
		COM_QuotedString(alias, quotedalias, sizeof(quotedalias), false);
		Key_ConsoleReplace(va("alias %s %s", Cmd_Argv(1), quotedalias));
	}
	else
		Con_Printf("Not an alias\n");
}

/*
===================
Key_Bind_f
===================
*/
void Key_Bind_f (void)
{
	int			i, c, b, modifier;
	char		cmd[1024];
	int bindmap = 0;

	if (!strcmp("in_bind", Cmd_Argv(0)))
	{
		bindmap = atoi(Cmd_Argv(1));
		Cmd_ShiftArgs(1, Cmd_ExecLevel==RESTRICT_LOCAL);
	}
	
	c = Cmd_Argc();

	if (c < 2)
	{
		Con_Printf ("bind <key> [command] : attach a command to a key\n");
		return;
	}
	b = Key_StringToKeynum (Cmd_Argv(1), &modifier);
	if (b==-1)
	{
		if (cl_warncmd.ival)
			Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}
	if (bindmap)
	{
		if (bindmap <= 0 || bindmap > KEY_MODIFIER_ALTBINDMAP)
		{
			if (cl_warncmd.ival)
				Con_Printf ("unsupported bindmap %i\n", bindmap);
			return;
		}
		if (modifier != ~0)
		{
			if (cl_warncmd.ival)
				Con_Printf ("modifiers cannot be combined with bindmaps\n");
			return;
		}
		modifier = (bindmap-1) | KEY_MODIFIER_ALTBINDMAP;
	}

	if (c == 2)
	{
		if (modifier == ~0)	//modifier unspecified. default to no modifier
			modifier = 0;
		if (keybindings[b][modifier])
		{
			char *alias = Cmd_AliasExist(keybindings[b][modifier], RESTRICT_LOCAL);
			char quotedbind[2048];
			char quotedalias[2048];
			COM_QuotedString(keybindings[b][modifier], quotedbind, sizeof(quotedbind), false);
			if (alias)
			{
				COM_QuotedString(alias, quotedalias, sizeof(quotedalias), false);
				Con_Printf ("^[\"%s\"\\type\\bind %s %s^] = ^[\"%s\"\\type\\alias %s %s^]\n", Cmd_Argv(1), Cmd_Argv(1), quotedbind, keybindings[b][modifier], keybindings[b][modifier], quotedalias);
			}
			else
				Con_Printf ("^[\"%s\"\\type\\bind %s %s^] = \"%s\"\n", Cmd_Argv(1), keybindings[b][modifier], Cmd_Argv(1), keybindings[b][modifier] );
		}
		else
			Con_Printf ("\"%s\" is not bound\n", Cmd_Argv(1) );
		return;
	}

	if (c > 3)
	{
		Cmd_ShiftArgs(1, Cmd_ExecLevel==RESTRICT_LOCAL);
		Key_SetBinding (b, modifier, Cmd_Args(), Cmd_ExecLevel);
		return;
	}
	
// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	for (i=2 ; i< c ; i++)
	{
		Q_strncatz (cmd, Cmd_Argv(i), sizeof(cmd));
		if (i != (c-1))
			Q_strncatz (cmd, " ", sizeof(cmd));
	}

	Key_SetBinding (b, modifier, cmd, Cmd_ExecLevel);
}

void Key_BindLevel_f (void)
{
	int			i, c, b, modifier;
	char		cmd[1024];
	
	c = Cmd_Argc();

	if (c != 2 && c != 4)
	{
		Con_Printf ("%s <key> [<level> <command>] : attach a command to a key for a specific level of access\n", Cmd_Argv(0));
		return;
	}
	b = Key_StringToKeynum (Cmd_Argv(1), &modifier);
	if (b==-1)
	{
		if (cl_warncmd.ival)
			Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	if (modifier == ~0)	//modifier unspecified. default to no modifier
		modifier = 0;

	if (c == 2)
	{
		if (keybindings[b][modifier])
			Con_Printf ("\"%s\" (%i)= \"%s\"\n", Cmd_Argv(1), bindcmdlevel[b][modifier], keybindings[b][modifier] );
		else
			Con_Printf ("\"%s\" is not bound\n", Cmd_Argv(1) );
		return;
	}

	if (Cmd_IsInsecure())
	{
		Con_Printf("Server attempted usage of %s\n", Cmd_Argv(0));
		return;
	}

// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	for (i=3 ; i< c ; i++)
	{
		Q_strncatz (cmd, Cmd_Argv(i), sizeof(cmd));
		if (i != (c-1))
			Q_strncatz (cmd, " ", sizeof(cmd));
	}

	Key_SetBinding (b, modifier, cmd, atoi(Cmd_Argv(2)));
}

/*
============
Key_WriteBindings

Writes lines containing "bind key value"
============
*/
void Key_WriteBindings (vfsfile_t *f)
{
	const char *s;
	int		i, m;
	char *binding, *base;

	char keybuf[256];
	char commandbuf[2048];

	for (i=0 ; i<K_MAX ; i++)	//we rebind the key with all modifiers to get the standard bind, then change the specific ones.
	{						//this does two things, it normally allows us to skip 7 of the 8 possibilities
		base = keybindings[i][0];	//plus we can use the config with other clients.
		if (!base)
			base = "";
		for (m = 0; m < KEY_MODIFIER_ALTBINDMAP; m++)
		{
			binding = keybindings[i][m];
			if (!binding)
				binding = "";
			if (strcmp(binding, base) || (m==0 && keybindings[i][0]) || bindcmdlevel[i][m] != bindcmdlevel[i][0])
			{
				s = Key_KeynumToString(i, m);
				//quote it as required
				if (i == ';' || i <= ' ' || strchr(s, ' ') || strchr(s, '+') || strchr(s, '\"'))
					s = COM_QuotedString(s, keybuf, sizeof(keybuf), false);

				if (bindcmdlevel[i][m] != RESTRICT_LOCAL)
					s = va("bindlevel %s %i %s\n", s, bindcmdlevel[i][m], COM_QuotedString(binding, commandbuf, sizeof(commandbuf), false));
				else
					s = va("bind %s %s\n", s, COM_QuotedString(binding, commandbuf, sizeof(commandbuf), false));
				VFS_WRITE(f, s, strlen(s));
			}
		}
		//now generate some special in_binds for bindmaps.
		for (m = 0; m < KEY_MODIFIER_ALTBINDMAP; m++)
		{
			binding = keybindings[i][m|KEY_MODIFIER_ALTBINDMAP];
			if (binding && *binding)
			{
				s = va("%s", Key_KeynumToString(i, 0));
				//quote it as required
				if (i == ';' || i <= ' ' || i == '\"')
					s = COM_QuotedString(s, keybuf, sizeof(keybuf), false);

				s = va("in_bind %i %s %s\n", m+1, s, COM_QuotedString(binding, commandbuf, sizeof(commandbuf), false));
				VFS_WRITE(f, s, strlen(s));
			}
		}
	}
}


/*
===================
Key_Init
===================
*/
void Key_Init (void)
{
	int		i;

	for (i=0 ; i<=CON_EDIT_LINES_MASK ; i++)
	{
		key_lines[i] = Z_Malloc(1);
		key_lines[i][0] = '\0';
	}
	key_linepos = 0;

	key_dest_mask = kdm_game;
	key_dest_absolutemouse = kdm_console | kdm_editor | kdm_cwindows | kdm_emenu;
	
//
// init ascii characters in console mode
//
	for (i=32 ; i<128 ; i++)
		consolekeys[i] = true;
	consolekeys[K_ENTER] = true;
	consolekeys[K_KP_ENTER] = true;
	consolekeys[K_TAB] = true;
	consolekeys[K_LEFTARROW] = true;
	consolekeys[K_RIGHTARROW] = true;
	consolekeys[K_UPARROW] = true;
	consolekeys[K_DOWNARROW] = true;
	consolekeys[K_KP_LEFTARROW] = true;
	consolekeys[K_KP_RIGHTARROW] = true;
	consolekeys[K_KP_UPARROW] = true;
	consolekeys[K_KP_DOWNARROW] = true;
	consolekeys[K_BACKSPACE] = true;
	consolekeys[K_DEL] = true;
	consolekeys[K_KP_DEL] = true;
	consolekeys[K_HOME] = true;
	consolekeys[K_KP_HOME] = true;
	consolekeys[K_END] = true;
	consolekeys[K_KP_END] = true;
	consolekeys[K_PGUP] = true;
	consolekeys[K_KP_PGUP] = true;
	consolekeys[K_PGDN] = true;
	consolekeys[K_KP_PGDN] = true;
	consolekeys[K_LSHIFT] = true;
	consolekeys[K_RSHIFT] = true;
	consolekeys[K_MWHEELUP] = true;
	consolekeys[K_MWHEELDOWN] = true;
	consolekeys[K_LCTRL] = true;
	consolekeys[K_RCTRL] = true;
	consolekeys[K_LALT] = true;
	consolekeys[K_RALT] = true;
	consolekeys['`'] = false;
	consolekeys['~'] = false;

	for (i=K_MOUSE1 ; i<K_MOUSE10 ; i++)
	{
		consolekeys[i] = true;
	}
	consolekeys[K_MWHEELUP] = true;
	consolekeys[K_MWHEELDOWN] = true;

	for (i=0 ; i<K_MAX ; i++)
		keyshift[i] = i;
	for (i='a' ; i<='z' ; i++)
		keyshift[i] = i - 'a' + 'A';
	keyshift['1'] = '!';
	keyshift['2'] = '@';
	keyshift['3'] = '#';
	keyshift['4'] = '$';
	keyshift['5'] = '%';
	keyshift['6'] = '^';
	keyshift['7'] = '&';
	keyshift['8'] = '*';
	keyshift['9'] = '(';
	keyshift['0'] = ')';
	keyshift['-'] = '_';
	keyshift['='] = '+';
	keyshift[','] = '<';
	keyshift['.'] = '>';
	keyshift['/'] = '?';
	keyshift[';'] = ':';
	keyshift['\''] = '"';
	keyshift['['] = '{';
	keyshift[']'] = '}';
	keyshift['`'] = '~';
	keyshift['\\'] = '|';

	menubound[K_ESCAPE] = true;
	for (i=0 ; i<12 ; i++)
		menubound[K_F1+i] = true;

//
// register our functions
//
	Cmd_AddCommand ("bind",Key_Bind_f);
	Cmd_AddCommand ("in_bind",Key_Bind_f);
	Cmd_AddCommand ("bindlevel",Key_BindLevel_f);
	Cmd_AddCommand ("unbind",Key_Unbind_f);
	Cmd_AddCommand ("unbindall",Key_Unbindall_f);
	Cmd_AddCommand ("aliasedit",Key_AliasEdit_f);

	Cvar_Register (&con_selectioncolour, "Console variables");
	Cvar_Register (&con_echochat, "Console variables");
}

qboolean Key_MouseShouldBeFree(void)
{
	//returns if the mouse should be a cursor or if it should go to the menu

	//if true, the input code is expected to return mouse cursor positions rather than deltas
	extern cvar_t cl_prydoncursor;
	if (key_dest_absolutemouse & key_dest_mask)
		return true;

	if (Key_Dest_Has(kdm_editor))
		return true;

//	if (!vid.activeapp)
//		return true;

	if (Key_Dest_Has(kdm_emenu))
		return true;

#ifdef VM_UI
	if (UI_MenuState())
		return false;
#endif

	if (Media_PlayingFullScreen())
		return true;

	if (cl_prydoncursor.ival)
		return true;

	return false;
}

/*
===================
Key_Event

Called by the system between frames for both key up and key down events
Should NOT be called during an interrupt!

On some systems, keys and (uni)char codes will be entirely separate events.
===================
*/
void Key_Event (unsigned int devid, int key, unsigned int unicode, qboolean down)
{
	int bl, bkey;
	char	*dc, *uc;
	char	p[16];
	int modifierstate;
	int conkey = consolekeys[key] || ((unicode || key == '`') && (key != '`' || key_linepos>0));	//if the input line is empty, allow ` to toggle the console, otherwise enter it as actual text.

//	Con_Printf ("%i : %i : %i\n", key, unicode, down); //@@@

	//bug: two of my keyboard doesn't fire release events if the other shift is already pressed (so I assume this is a common thing).
	//hack around that by just force-releasing eg left if right is pressed, but only on inital press to avoid potential infinite loops if the state got bad.
	//ctrl+alt don't seem to have the problem.
	if (key == K_LSHIFT && !keydown[K_LSHIFT] && keydown[K_RSHIFT])
		Key_Event(devid, K_RSHIFT, 0, false);
	if (key == K_RSHIFT && !keydown[K_RSHIFT] && keydown[K_LSHIFT])
		Key_Event(devid, K_LSHIFT, 0, false);

	modifierstate = KeyModifier(keydown[K_LSHIFT]|keydown[K_RSHIFT], keydown[K_LALT]|keydown[K_RALT], keydown[K_LCTRL]|keydown[K_RCTRL]);

	keydown[key] = down;

	if (!down)
		key_repeats[key] = 0;

	key_lastpress = key;
	key_count++;
	if (key_count <= 0)
	{
		return;		// just catching keys for Con_NotifyBox
	}

// update auto-repeat status
	if (down)
	{
		key_repeats[key]++;
			
//		if (key >= 200 && !keybindings[key])	//is this too annoying?
//			Con_Printf ("%s is unbound, hit F4 to set.\n", Key_KeynumToString (key) );
	}

	if (key == K_LSHIFT || key == K_RSHIFT)
	{
		shift_down = keydown[K_LSHIFT]|keydown[K_RSHIFT];
	}

	if (key == K_ESCAPE)
	{
		if (shift_down)
		{
			extern cvar_t con_stayhidden;
			if (down && con_stayhidden.ival < 2)
			{
				if (!Key_Dest_Has(kdm_console))	//don't toggle it when the console is already down. this allows typing blind to not care if its already active.
					Con_ToggleConsole_Force();
				return;
			}
		}
	}

	//yes, csqc is allowed to steal the escape key.
	if (key != '`' && (!down || key != K_ESCAPE || (!Key_Dest_Has(~kdm_game) && !shift_down)) &&
		!Key_Dest_Has(~kdm_game) && !Media_PlayingFullScreen())
	{
#ifdef CSQC_DAT
		if (CSQC_KeyPress(key, unicode, down, devid))	//give csqc a chance to handle it.
			return;
#endif
#ifdef VM_CG
		if (CG_KeyPress(key, unicode, down))
			return;
#endif
	}
	else if (!down && key)
	{
#ifdef CSQC_DAT
		//csqc should still be told of up events. note that there's some filering to prevent notifying about events that it shouldn't receive (like all the up events when typing at the console).
		CSQC_KeyPress(key, unicode, down, devid);
#endif
	}

//
// handle escape specialy, so the user can never unbind it
//
	if (key == K_ESCAPE)
	{
#ifdef VM_UI
#ifdef TEXTEDITOR
		if (!Key_Dest_Has(~kdm_game) && !Key_Dest_Has(kdm_console))
#endif
		{
			if (down && Media_PlayingFullScreen())
			{
				Media_StopFilm(false);
				return;
			}
			if (UI_KeyPress(key, unicode, down))	//Allow the UI to see the escape key. It is possible that a developer may get stuck at a menu.
				return;
		}
#endif

		if (!down)
		{
#ifdef MENU_DAT
			if (Key_Dest_Has(kdm_gmenu) && !Key_Dest_Has(kdm_editor|kdm_console|kdm_cwindows))
				MP_Keyup (key, unicode, devid);
#endif
			return;
		}

		if (Key_Dest_Has(kdm_console))
		{
			Key_Dest_Remove(kdm_console);
			Key_Dest_Remove(kdm_cwindows);
			if (!cls.state && !Key_Dest_Has(~kdm_game) && !Media_PlayingFullScreen())
				M_ToggleMenu_f ();
		}
		else if (Key_Dest_Has(kdm_cwindows))
		{
			Key_Dest_Remove(kdm_cwindows);
			if (!cls.state && !Key_Dest_Has(~kdm_game) && !Media_PlayingFullScreen())
				M_ToggleMenu_f ();
		}
#ifdef TEXTEDITOR
		else if (Key_Dest_Has(kdm_editor))
			Editor_Key (key, unicode);
#endif
		else if (Key_Dest_Has(kdm_emenu))
			M_Keydown (key, unicode);
#ifdef MENU_DAT
		else if (Key_Dest_Has(kdm_gmenu))
			MP_Keydown (key, unicode, devid);
#endif
		else if (Key_Dest_Has(kdm_message))
			Key_Dest_Remove(kdm_message);
		else
		{
			if (Media_PlayingFullScreen())
			{
				Media_StopFilm(true);
				if (!cls.state)
					M_ToggleMenu_f ();
			}
			else
				M_ToggleMenu_f ();
		}
		return;
	}

//
// key up events only generate commands if the game key binding is
// a button command (leading + sign).  These will occur even in console mode,
// to keep the character from continuing an action started before a console
// switch.  Button commands include the keynum as a parameter, so multiple
// downs can be matched with ups
//
	if (!down)
	{
		if (Key_Dest_Has(kdm_console|kdm_cwindows))
		{
			console_t *con = Key_Dest_Has(kdm_console)?con_current:con_curwindow;
			if (con_mouseover && key >= K_MOUSE1 && key <= K_MWHEELDOWN)
				con = con_mouseover;
			if (con_curwindow && con_curwindow != con)
				con_curwindow->buttonsdown = CB_NONE;
			if (con)
			{
				con->mousecursor[0] = mousecursor_x - ((con->flags & CONF_ISWINDOW)?con->wnd_x+8:0);
				con->mousecursor[1] = mousecursor_y - ((con->flags & CONF_ISWINDOW)?con->wnd_y:0);
				Key_ConsoleRelease(con, key, unicode);
			}
		}
		if (Key_Dest_Has(kdm_emenu))
			M_Keyup (key, unicode);
#ifdef MENU_DAT
		if (Key_Dest_Has(kdm_gmenu))
			MP_Keyup (key, unicode, devid);
#endif
#ifdef HAVE_MEDIA_DECODER
		if (Media_PlayingFullScreen())
			Media_Send_KeyEvent(NULL, key, unicode, down?0:1);
#endif

		uc = releasecommand[key][devid%MAX_INDEVS];
		if (uc)	//this wasn't down, so don't crash on bad commands.
		{
			releasecommand[key][devid%MAX_INDEVS] = NULL;
			Cbuf_AddText (uc, releasecommandlevel[key][devid%MAX_INDEVS]);
			Z_Free(uc);
		}
		return;
	}

//
// during demo playback, most keys bring up the main menu
//
	if (cls.demoplayback && cls.demoplayback != DPB_MVD && cls.demoplayback != DPB_EZTV && conkey && !Key_Dest_Has(~kdm_game))
	{
		switch (key)
		{	//these keys don't force the menu to appear while playing the demo reel
		case K_LSHIFT:
		case K_RSHIFT:
		case K_LALT:
		case K_RALT:
		case K_LCTRL:
//		case K_RCTRL:
			break;
		default:
			dc = keybindings[key][modifierstate];
			//toggleconsole or +showFOO keys should do their regular bind action
			if (!dc || (strcmp(dc, "toggleconsole") && strncmp(dc, "+show", 5)))
			{
				M_ToggleMenu_f ();
				return;
			}
		}
	}

//
// if not a consolekey, send to the interpreter no matter what mode is
//
	if (/*conkey &&*/Key_Dest_Has(kdm_console|kdm_cwindows))
	{
		console_t *con = Key_Dest_Has(kdm_console)?con_current:con_curwindow;
		if ((con_mouseover||!Key_Dest_Has(kdm_console)) && key >= K_MOUSE1 && key <= K_MWHEELDOWN)
			con = con_mouseover;
		if (con)
		{
			con->mousecursor[0] = mousecursor_x - ((con->flags & CONF_ISWINDOW)?con->wnd_x+8:0);
			con->mousecursor[1] = mousecursor_y - ((con->flags & CONF_ISWINDOW)?con->wnd_y:0);
			if (Key_Console (con, unicode, key))
				return;
		}
		else
			Key_Dest_Remove(kdm_cwindows);

	}
#ifdef HAVE_MEDIA_DECODER
	if (Media_PlayingFullScreen())
	{
		Media_Send_KeyEvent(NULL, key, unicode, down?0:1);
		return;
	}
#endif
#ifdef TEXTEDITOR
	if (Key_Dest_Has(kdm_editor))
	{
		Editor_Key (key, unicode);
		return;
	}
#endif
#ifdef VM_UI
	if (!Key_Dest_Has(~kdm_game) || !down)
	{
		if (UI_KeyPress(key, unicode, down) && down)	//UI is allowed to take these keydowns. Keyups are always maintained.
			return;
	}
#endif

	if (Key_Dest_Has(kdm_emenu))
	{
		M_Keydown (key, unicode);
		return;
	}
#ifdef MENU_DAT
	if (Key_Dest_Has(kdm_gmenu))
	{
		if (MP_Keydown (key, unicode, devid))
			return;
	}
#endif
	if (Key_Dest_Has(kdm_message))
	{
		Key_Message (key, unicode);
		return;
	}

	//anything else is a key binding.

	/*don't auto-repeat binds as it breaks too many scripts*/
	if (key_repeats[key] > 1)
		return;

	//first player is normally assumed anyway.
	if (cl_forceseat.ival>0)
		Q_snprintfz (p, sizeof(p), "p %i ", cl_forceseat.ival);
	else if (devid)
		Q_snprintfz (p, sizeof(p), "p %i ", devid+1);
	else
		*p = 0;

	//assume the worst
	dc = NULL;
	bl = 0;
	//try bindmaps if they're set
	if (key_bindmaps[0] && (!dc || !*dc))
	{
		dc = keybindings[key][key_bindmaps[0]];
		bl = bindcmdlevel[key][key_bindmaps[0]];
	}
	if (key_bindmaps[1] && (!dc || !*dc))
	{
		dc = keybindings[key][key_bindmaps[1]];
		bl = bindcmdlevel[key][key_bindmaps[1]];
	}

	//regular ctrl_alt_shift_foo binds
	if (!dc || !*dc)
	{
		dc = keybindings[key][modifierstate];
		bl = bindcmdlevel[key][modifierstate];
	}

	//simulate singular shift+alt+ctrl for binds (no left/right). really though, this code should translate to csqc/menu keycodes and back to resolve the weirdness instead.
	if (key == K_RALT && (!dc || !*dc))
	{
		bkey = K_LALT;
		dc = keybindings[bkey][modifierstate];
		bl = bindcmdlevel[bkey][modifierstate];
	}
	else if (key == K_RCTRL && (!dc || !*dc))
	{
		bkey = K_LCTRL;
		dc = keybindings[bkey][modifierstate];
		bl = bindcmdlevel[bkey][modifierstate];
	}
	else if (key == K_RSHIFT && (!dc || !*dc))
	{
		bkey = K_LSHIFT;
		dc = keybindings[bkey][modifierstate];
		bl = bindcmdlevel[bkey][modifierstate];
	}
	else
		bkey = key;

	if (key == K_MOUSE1 && IN_MouseDevIsTouch(devid))
	{
		char *button = SCR_ShowPics_ClickCommand(mousecursor_x, mousecursor_y);
		if (button)
		{
			dc = button;
			bl = RESTRICT_INSECURE;
		}
		else
		{
			int bkey = Sbar_TranslateHudClick();
			if (bkey)
			{
				dc = keybindings[bkey][modifierstate];
				bl = bindcmdlevel[bkey][modifierstate];
			}
			else
			{
				bkey = IN_TranslateMButtonPress(devid);
				if (bkey)
				{
					dc = keybindings[bkey][modifierstate];
					bl = bindcmdlevel[bkey][modifierstate];
				}
				else if (!Key_MouseShouldBeFree())
				{
					key_repeats[key] = 0;
					return;
				}
			}
		}
	}

	if (dc)
	{
		if (dc[0] == '+')
		{
			uc = va("-%s%s %i\n", p, dc+1, bkey);
			dc = va("+%s%s %i\n", p, dc+1, bkey);
		}
		else
		{
			uc = NULL;
			dc = va("%s%s\n", p, dc);
		}
	}
	else
		uc = NULL;

	//don't mess up if we ran out of devices, just silently release the one that it conflicted with (and only if its in conflict).
	if (releasecommand[key][devid%MAX_INDEVS] && (!uc || strcmp(uc, releasecommand[key][devid%MAX_INDEVS])))
	{
		Cbuf_AddText (releasecommand[key][devid%MAX_INDEVS], releasecommandlevel[key][devid%MAX_INDEVS]);
		Z_Free(releasecommand[key][devid%MAX_INDEVS]);
		releasecommand[key][devid%MAX_INDEVS] = NULL;
	}
	if (dc)
		Cbuf_AddText (dc, bl);
	if (uc)
		releasecommand[key][devid%MAX_INDEVS] = Z_StrDup(uc);
	releasecommandlevel[key][devid%MAX_INDEVS] = bl;
}

/*
===================
Key_ClearStates
===================
*/
void Key_ClearStates (void)
{
	int		i;

	for (i=0 ; i<K_MAX ; i++)
	{
		keydown[i] = false;
		key_repeats[i] = false;
	}
}

