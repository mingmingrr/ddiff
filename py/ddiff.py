#!/usr/bin/env python3

from __future__ import annotations

from textual.app import *
from textual.widgets import *
from textual.containers import *
from textual.reactive import *
from textual.binding import *
from textual.color import *
from textual.css.styles import *
import textual.css.parse as parse

from rich.style import Style
from rich.color import Color as RichColor

import re
import os
import shlex
import argparse
import enum
import dataclasses
import subprocess
import filecmp
import natsort
import asyncio
from pathlib import Path

# import rich.traceback
# rich.traceback.install(show_locals=True)

logfile = Path('ddiff.log')
def trace(*args, **kwargs):
	if logfile.exists():
		with logfile.open('a') as file:
			print(*args, **kwargs, file=file)
	return args[-1]

class FileType(enum.Enum):
	Normal      = 'no'
	Missing     = 'mi'
	File        = 'fi'
	Setuid      = 'su'
	Setgid      = 'sg'
	Executable  = 'ex'
	Directory   = 'di'
	Sticky      = 'st'
	StickyWrite = 'tw'
	OtherWrite  = 'ow'
	Symlink     = 'ln'
	Orphan      = 'or'
	BlockDevice = 'bd'
	CharDevice  = 'cd'
	NamedPipe   = 'pi'
	Socket      = 'so'
	Door        = 'do'
	Unknown     = 'uk'

class Status(enum.Enum):
	Matching  = '  '
	Unknown   = '??'
	Different = '**'
	LeftOnly  = '+-'
	RightOnly = '-+'

parser = argparse.ArgumentParser()
parser.add_argument('-e', '--editor',
	default='$EDITOR -d',
	help='program used to diff two files (default: %(default)r)')
parser.add_argument('-x', '--exclude',
	default=[r'^\b$'], action='append', metavar='REGEX',
	help='ignore files matching regex')
# parser.add_argument('-s', '--sort',
	# default='natural',
	# help='Order to sort files',
	# choices=['natural', 'lexicographic', 'numeric'])
parser.add_argument('left', metavar='DIR1', type=Path)
parser.add_argument('right', metavar='DIR2', type=Path)

def file_type(path:Path):
	path = Path(path)
	stat = path.stat()
	if path.is_symlink():
		if not path.exists(): return FileType.Orphan
		return FileType.Symlink
	if not path.exists(): return FileType.Missing
	if path.is_dir():
		sticky = stat.st_mode & 0o01000
		write = stat.st_mode & 0o00002
		if sticky and write: return FileType.StickyWrite
		if write: return FileType.OtherWrite
		if sticky: return FileType.Sticky
		return FileType.Directory
	if path.is_mount(): return FileType.Door
	if path.is_block_device(): return FileType.BlockDevice
	if path.is_char_device(): return FileType.CharDevice
	if path.is_fifo(): return FileType.NamedPipe
	if path.is_socket(): return FileType.Socket
	if not path.is_file(): return FileType.Orphan
	if stat.st_mode & 0o04000: return FileType.Setuid
	if stat.st_mode & 0o02000: return FileType.Setgid
	if stat.st_mode & 0o00111: return FileType.Executable
	return FileType.File

class DirDiffEntry(ListItem):
	DEFAULT_CSS = '''
		ListView > DirDiffEntry.--highlight { background: $accent 33%; }
		ListView:focus > DirDiffEntry.--highlight { background: $accent 33%; }
		DirDiffEntry { background: $background 0%; }
		DirDiffEntry .icon { width: 1; margin-right: 1; text-style: bold; color: black; }
		DirDiffEntry .type { color: $text-muted; margin-right: 1; }
		DirDiffEntry .side { width: 1fr; }
		DirDiffEntry .left { border-right: solid $primary; }
		DirDiffEntry.different .icon { background: $warning; }
		DirDiffEntry.unknown   .icon { background: $secondary; }
		DirDiffEntry.leftonly  .right .icon { background: $error; }
		DirDiffEntry.leftonly  .right .type { width: 0; }
		DirDiffEntry.leftonly  .right .name { width: 0; }
		DirDiffEntry.rightonly .left  .icon { background: $error; }
		DirDiffEntry.rightonly .left  .type { width: 0; }
		DirDiffEntry.rightonly .left  .name { width: 0; }
		DirDiffEntry.leftonly  .left  .icon { background: $success; }
		DirDiffEntry.rightonly .right .icon { background: $success; }
	'''
	name   = var(None)
	status = reactive(Status.Unknown, always_update=True, init=False)
	left   = reactive(FileType.Unknown, always_update=True, init=False)
	right  = reactive(FileType.Unknown, always_update=True, init=False)
	def __init__(self, name, *args, status=Status.Unknown,
			left=FileType.Unknown, right=FileType.Unknown, **kwargs):
		super().__init__(Horizontal(self.make_side(name, 'left'),
			self.make_side(name, 'right')), *args, **kwargs)
		self.name = name
		self.status = status
		self.left = left
		self.right = right
	@staticmethod
	def make_side(name, side):
		return Horizontal(
			Label('', classes='icon'),
			Label('', classes='type'),
			Label(name, classes='name'),
			classes='side {}'.format(side))
	def watch_left(self, old, new):
		self.watch_side('left', old, new)
	def watch_right(self, old, new):
		self.watch_side('right', old, new)
	def watch_side(self, side, old, new):
		side = self.query_one('.{}'.format(side))
		name = side.query_one('.name')
		name.remove_class('type-{}'.format(old.value))
		name.add_class('type-{}'.format(new.value))
		side.query_one('.type').update(new.value)
	def watch_status(self, old, new):
		self.remove_class(old.name.lower())
		self.add_class(new.name.lower())
		left = self.query_one('.left')
		right = self.query_one('.right')
		left.query_one('.icon').update(new.value[0])
		right.query_one('.icon').update(new.value[1])

class DirDiffApp(App):
	TITLE = 'DirDiff'
	BINDINGS = [
		Binding('q', 'quit', 'quit'),
		Binding('e,right', 'select', 'select', priority=True, key_display='▶'),
		Binding('escape,left', 'leave', 'leave', priority=True, key_display='◀'),
		Binding('r', 'refresh', 'refresh'),
		Binding('s', 'shell(0)', 'shell-left', key_display='s'),
		Binding('S', 'shell(1)', 'shell-right', key_display='S'),
	]
	CSS = '''
		$background: #000;
		#paths { height: 1; overflow-y: scroll; }
		#paths Label { width: 1fr }
		#paths .left { border-right: solid $primary; }
		#files { background: $background; height: 1fr; overflow-y: scroll; }
	'''
	conf = var(None)
	cwd = reactive(Path('.'), always_update=True)
	def __init__(self, *args, config=None, **kwargs):
		super().__init__(*args, **kwargs)
		assert config is not None
		self.conf = config
	def compose(self):
		yield Header(show_clock=True)
		with Horizontal(id='paths'):
			yield Label(str(self.conf.left), classes='left')
			yield Label(str(self.conf.right), classes='right')
		yield ListView(id='files')
		yield Footer()
	def on_mount(self):
		self.query_one('#files').focus()
	async def watch_cwd(self, old, new):
		left = self.conf.left / self.cwd
		right = self.conf.right / self.cwd
		self.query_one('#paths .left').update(str(left))
		self.query_one('#paths .right').update(str(right))
		files = self.query_one('#files')
		files.clear()
		async for name, status, l, r in diff_dir(left, right, self.conf.exclude):
			files.append(DirDiffEntry(name, status=status, left=l, right=r))
	def action_select(self):
		self.query_one('#files').action_select_cursor()
	def on_list_view_selected(self, message):
		path = self.cwd / message.item.name
		left, right = self.conf.left / path, self.conf.right / path
		cmd = '{} {} {}'.format(self.conf.editor,
			shlex.quote(str(left)), shlex.quote(str(right)))
		if left.is_dir() and right.is_dir() and left.name == right.name:
			self.cwd = self.cwd / left.name
			return
		with self.suspend():
			subprocess.run(cmd, shell=True,
				stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)
		self.watch_cwd(self.cwd, self.cwd)
	def action_leave(self):
		if self.cwd == Path('.'): return
		self.cwd = self.cwd.parent
	def action_refresh(self):
		self.watch_cwd(self.cwd, self.cwd)
	def action_shell(self, index):
		path = [self.conf.left, self.conf.right][index] / self.cwd
		with self.suspend():
			subprocess.run(os.environ.get('SHELL', 'sh'), shell=True, cwd=path,
				stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)

async def diff_dir(left, right, exclude):
	lefts = natsort.natsorted(left.iterdir(), reverse=True)
	rights = natsort.natsorted(right.iterdir(), reverse=True)
	async for i in diff_files(lefts, rights, exclude): yield i

async def diff_files(lefts, rights, exclude):
	while lefts and rights:
		await asyncio.sleep(0)
		l, r = lefts[-1], rights[-1]
		lk, rk = natsort.natsort_key(l.name), natsort.natsort_key(r.name)
		if lk == rk and l.name == r.name:
			lefts.pop(), rights.pop()
			if exclude.match(l.name) is not None: continue
			yield (l.name, *(await diff_file(l, r, exclude)))
		elif lk < rk or (lk == rk and l.name < r.name):
			lefts.pop()
			if exclude.match(l.name) is not None: continue
			yield (l.name, Status.LeftOnly,
				file_type(l), FileType.Missing)
		else:
			rights.pop()
			if exclude.match(r.name) is not None: continue
			yield (r.name, Status.RightOnly,
				FileType.Missing, file_type(r))
	for l in lefts:
		if exclude.match(l.name) is not None: continue
		yield (l.name, Status.LeftOnly,
			file_type(l), FileType.Missing)
	for r in rights:
		if exclude.match(r.name) is not None: continue
		yield (r.name, Status.RightOnly,
			FileType.Missing, file_type(r))

async def diff_file(left, right, exclude):
	lstat, rstat = left.stat(), right.stat()
	ltype, rtype = file_type(left), file_type(right)
	if lstat.st_dev == rstat.st_dev and lstat.st_ino == rstat.st_ino:
		return (Status.Matching, ltype, rtype)
	if left.is_symlink():
		status, left, right = await diff_file(left.resolve(), right, exclude)
		return (status, ltype, right)
	if right.is_symlink():
		status, left, right = await diff_file(left, right.resolve(), exclude)
		return (status, left, rtype)
	if left.is_dir() and right.is_dir():
		status = Status.Matching
		async for entry in diff_dir(left, right, exclude):
			if entry[1] == Status.Matching:
				continue
			if entry[1] == Status.Unknown:
				status = Status.Unknown
				continue
			return (Status.Different, ltype, rtype)
		return (status, ltype, rtype)
	if left.is_file() and right.is_file():
		if filecmp.cmp(left, right):
			return (Status.Matching, ltype, rtype)
		return (Status.Different, ltype, rtype)
	if ltype != rtype:
		return (Status.Different, ltype, rtype)
	return (Status.Unknown, ltype, rtype)

def ansi_style(nums):
	style, fg, bg = {}, None, None
	nums = list(map(int, reversed(nums.split(';'))))
	while nums:
		n = nums.pop()
		s = ansi_styles.get(n, None)
		if s is not None: style[s] = True
		elif 30 <= n <= 37: fg = ansi_colors[n - 30]
		elif 90 <= n <= 97: fg = ansi_colors[n - 82]
		elif 40 <= n <= 47: bg = ansi_colors[n - 40]
		elif 100 <= n <= 107: bg = ansi_colors[n - 92]
		elif n in [38, 48, 58]:
			m, r = nums.pop(), nums.pop()
			assert m in [2, 5], 'unknown color: {}'.format(n)
			if m == 2: ansi_colors[r] if r < 16 else RichColor.from_ansi(r)
			elif m == 5: c = RichColor(r, nums.pop(), nums.pop())
			fg, bg = c if n == 38 else fg, c if n == 48 else bg
	return style, fg, bg

ansi_styles = {
	1: 'bold',
	2: 'dim',
	3: 'italic',
	4: 'underline',
	5: 'blink',
	6: 'blink2',
	7: 'reverse',
	8: 'conceal',
	9: 'strike',
	21: 'underline2',
	51: 'frame',
	52: 'encircle',
	53: 'overline',
}

ansi_colors = [RichColor.parse(i) for i in
	('#000000 #cc0000 #00cc00 #cccc00'
	' #0077cc #cc00dd #00cccc #cccccc'
	' #777777 #ff2222 #22ff00 #ffff00'
	' #22aaff #ff22dd #00ffff #ffffff').split()]

def main():
	args = parser.parse_args()
	args.exclude = re.compile('|'.join(
		'(?:{})'.format(re.compile(i).pattern) for i in args.exclude))
	trace('-' * 80)
	colors = {
		'rs':'0',        'di':'01;34',    'ln':'01;36',    'mh':'00',
		'pi':'40;33',    'so':'01;35',    'bd':'40;33;01', 'do':'01;35',
		'cd':'40;33;01', 'or':'40;31;01', 'mi':'00',       'su':'37;41',
		'sg':'30;43',    'ca':'00',       'tw':'30;42',    'ow':'34;42',
		'st':'37;44',    'ex':'01;32',    'uk':'02;03', }
	colors.update(x.split('=', maxsplit=1) for x in
		os.environ.get('LS_COLORS', '').split(':') if x)
	styles = [DirDiffApp.CSS]
	for k, v in colors.items():
		css = 'DirDiffEntry Label.type-{} {{}}'.format(k)
		css = next(parse.parse(css, '<generated: {}>'.format(__file__)))
		style, fg, bg = ansi_style(v)
		if style: css.styles.set_rule('text_style', Style(**style))
		if fg: css.styles.set_rule('color', Color.from_rich_color(fg))
		if bg: css.styles.set_rule('color', Color.from_rich_color(bg))
		if fg or bg: css.styles.set_rule('auto_color', False)
		styles.append(css.css)
	DirDiffApp.CSS = '\n'.join(styles)
	app = DirDiffApp(config=args)
	app.run()

if __name__ == '__main__': main()
