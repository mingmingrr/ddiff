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

import stat
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
import itertools
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
	try: mode = path.stat().st_mode
	except FileNotFoundError:
		if stat.S_ISLNK(mode): return FileType.Orphan
		return FileType.Missing
	if stat.S_ISLNK(mode): return FileType.Symlink
	if stat.S_ISDIR(mode):
		sticky = mode & stat.S_ISVTX
		write = mode & stat.S_IWOTH
		if sticky and write: return FileType.StickyWrite
		if write: return FileType.OtherWrite
		if sticky: return FileType.Sticky
		return FileType.Directory
	if stat.S_ISDOOR(mode): return FileType.Door
	if stat.S_ISBLK(mode): return FileType.BlockDevice
	if stat.S_ISCHR(mode): return FileType.CharDevice
	if stat.S_ISFIFO(mode): return FileType.NamedPipe
	if stat.S_ISSOCK(mode): return FileType.Socket
	if not stat.S_ISREG(mode): return FileType.Unknown
	if mode & stat.S_ISUID: return FileType.Setuid
	if mode & stat.S_ISGID: return FileType.Setgid
	executable = stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
	if mode & executable: return FileType.Executable
	return FileType.File

class DiffEntry(ListItem):
	DEFAULT_CSS = '''
		ListView       > DiffEntry.--highlight { background: $accent 33%; }
		ListView:focus > DiffEntry.--highlight { background: $accent 33%; }
		DiffEntry { background: $background 0%; }
		DiffEntry .icon { width: 1; margin-right: 1; text-style: bold; color: black; }
		DiffEntry .type { color: $text-muted; margin-right: 1; }
		DiffEntry .side { width: 1fr; }
		DiffEntry .left { border-right: solid $primary; }
		DiffEntry.different .icon { background: $warning; }
		DiffEntry.unknown   .icon { background: $primary-background; }
		DiffEntry.leftonly  .right .icon { background: $error; }
		DiffEntry.leftonly  .right .type { width: 0; }
		DiffEntry.leftonly  .right .name { width: 0; }
		DiffEntry.rightonly .left  .icon { background: $error; }
		DiffEntry.rightonly .left  .type { width: 0; }
		DiffEntry.rightonly .left  .name { width: 0; }
		DiffEntry.leftonly  .left  .icon { background: $success; }
		DiffEntry.rightonly .right .icon { background: $success; }
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
			Label('?', classes='icon'),
			Label('??', classes='type'),
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
		Binding('s', 'shell("left")', 'shell-left', key_display='s'),
		Binding('S', 'shell("right")', 'shell-right', key_display='S'),
	]
	CSS = '''
		$background: #000;
		#paths { height: 1; overflow-y: scroll; }
		#paths Label { width: 1fr }
		#paths .left { border-right: solid $primary; }
		#files { background: $background; height: 1fr; overflow-y: scroll; }
	'''
	cwd = reactive(Path('.'), always_update=True)
	def __init__(self, *args, config=None, **kwargs):
		super().__init__(*args, **kwargs)
		assert config is not None
		self.conf = config
		self.index_cache = dict()
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
		entries = dict()
		lefts, rights = set(left.iterdir()), set(right.iterdir())
		sortedfiles = natsort.natsorted(set(i.name
			for i in itertools.chain(lefts, rights)))
		for index, name in enumerate(sortedfiles):
			if self.conf.exclude.search(name) is not None: continue
			l, r = left.joinpath(name) in lefts, right.joinpath(name) in rights
			if l and r: entry = entries[name] = DiffEntry(name,
				left=file_type(left / name), right=file_type(right / name))
			elif l: entry = DiffEntry(name, status=Status.LeftOnly,
				left=file_type(left / name), right=FileType.Missing)
			else: entry = DiffEntry(name, status=Status.RightOnly,
				left=FileType.Missing, right=file_type(right / name))
			append = files.append(entry)
			if index % 4 == 0: await append
		await asyncio.sleep(0)
		files.index = self.index_cache.get(self.cwd, 0)
		for name, entry in entries.items():
			entry.status = diff_file(left / name, right / name, self.conf.exclude)
			await asyncio.sleep(0)
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
	def on_list_view_highlighted(self, message):
		self.index_cache[self.cwd] = message.list_view.index
	def action_leave(self):
		if self.cwd == Path('.'): return
		self.cwd = self.cwd.parent
	def action_refresh(self):
		self.cwd = self.cwd
	def action_shell(self, side):
		path = getattr(self.conf, side) / self.cwd
		with self.suspend():
			subprocess.run(os.environ.get('SHELL', 'sh'), shell=True, cwd=path,
				stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)

def diff_dir(left, right, exclude):
	lefts = [i for i in left.iterdir() if exclude.match(i.name) is None]
	rights = [i for i in right.iterdir() if exclude.match(i.name) is None]
	if len(lefts) != len(rights): return Status.Different
	lefts.sort(), rights.sort()
	for l, r in zip(lefts, rights):
		if l.name != r.name: return Status.Different
	status = Status.Matching
	for l, r in zip(lefts, rights):
		entry = diff_file(l, r, exclude)
		if entry == Status.Matching: continue
		if entry == Status.Unknown:
			status = Status.Unknown ; continue
		return Status.Different
	return status

def diff_file(left, right, exclude):
	lstat, rstat = left.stat(), right.stat()
	if lstat.st_dev == rstat.st_dev and lstat.st_ino == rstat.st_ino:
		return Status.Matching
	lmode, rmode = lstat.st_mode, rstat.st_mode
	if stat.S_ISLNK(lmode):
		return diff_file(left.resolve(), right, exclude)
	if stat.S_ISLNK(rmode):
		return diff_file(left, right.resolve(), exclude)
	if stat.S_ISDIR(lmode) and stat.S_ISDIR(rmode):
		return diff_dir(left, right, exclude)
	if stat.S_ISREG(lmode) and stat.S_ISREG(rmode):
		if filecmp.cmp(left, right):
			return Status.Matching
		return Status.Different
	if ltype != rtype:
		return Status.Different
	return Status.Unknown

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
			if m == 5: c = ansi_colors[r] if r < 16 else RichColor.from_ansi(r)
			elif m == 2: c = RichColor(r, nums.pop(), nums.pop())
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
		if k.startswith('*'): continue
		css = next(parse.parse('DiffEntry .type-{} {{}}'.format(k),
			'<generated: {}>'.format(__file__)))
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
