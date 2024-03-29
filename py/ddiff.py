#!/usr/bin/env python3

from __future__ import annotations

from textual.app import *
from textual.widgets import *
from textual.containers import *
from textual.reactive import *
from textual.binding import *
from textual.screen import *
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
import shutil
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

class BindingDesc(Binding):
	def __init__(self, *args, long_description=None, **kwargs):
		super().__init__(*args, **kwargs)
		self.long_description = long_description

class MenuScreen(ModalScreen):
	BINDINGS = [
		BindingDesc('?,escape', 'pop_screen', 'exit menu', key_display='?'),
	]
	DEFAULT_CSS = '''
		MenuScreen .key { margin-right: 2; }
	'''
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		self.bindings = {(
			getattr(binding, 'key_display', None) or binding.key,
			getattr(binding, 'long_description', None) or binding.description)
			for binding in self.app.BINDINGS}
	def compose(self):
		with ListView() as container:
			container.border_title = 'Menu'
			for binding in DirDiffApp.BINDINGS:
				key = binding.key_display or binding.key
				desc = binding.long_description or binding.description
				with ListItem() as item:
					yield Horizontal(
						Label(key, classes='key'),
						Label(desc, classes='desc'))
					item.binding = binding
			container.focus()
	async def on_list_view_selected(self, message):
		self.app.pop_screen()
		key = message.item.binding.key.split(',', maxsplit=1)[0]
		await self.app.check_bindings(key, True) \
			or await self.app.check_bindings(key, False)
		message.stop()

class MessageScreen(ModalScreen):
	BINDINGS = [
		BindingDesc('escape', 'exit_menu', 'exit menu', key_display='Esc'),
	]
	DEFAULT_CSS = '''
		MessageScreen Widget { width: 100%; }
		MessageScreen .message { height: 1fr; content-align: center middle; }
		MessageScreen Button { background: $primary; }
	'''
	def __init__(self, title, message, *args, confirm='Ok', **kwargs):
		super().__init__(*args, **kwargs)
		self.done = asyncio.Event()
		self.title = title
		self.message = message
		self.confirm = confirm
	def compose(self):
		with Vertical() as container:
			container.border_title = self.title
			yield Label(self.message, classes='message')
			with Button(self.confirm) as button:
				button.focus()
	def finish(self):
		self.app.pop_screen()
		self.done.set()
	def on_button_pressed(self, message):
		self.finish()
	def action_exit_menu(self):
		self.finish(False)

class ConfirmScreen(ModalScreen):
	BINDINGS = [
		BindingDesc('escape', 'exit_menu', 'exit menu', key_display='Esc'),
		BindingDesc('left', 'focus_previous', 'focus previous', show=False),
		BindingDesc('right', 'focus_next', 'focus next', show=False),
	]
	DEFAULT_CSS = '''
		ConfirmScreen Grid {
			grid-size: 2;
			grid-rows: 1fr 3;
		}
		ConfirmScreen .message {
			column-span: 2;
			height: 100%;
			width: 100%;
			content-align: center middle;
		}
		ConfirmScreen Button { width: 100%; }
		ConfirmScreen .confirm { background: $primary; }
		ConfirmScreen .cancel { background: $secondary; }
	'''
	def __init__(self, title, message, *args,
			confirm='Ok', cancel='Cancel', default=False, **kwargs):
		super().__init__(*args, **kwargs)
		self.done = asyncio.Event()
		self.title = title
		self.message = message
		self.confirm = confirm
		self.cancel = cancel
		self.default = default
		self.result = default
	def compose(self):
		with Grid() as grid:
			grid.border_title = self.title
			yield Label(self.message, classes='message')
			left, right = (
				Button(self.cancel, classes='cancel'),
				Button(self.confirm, classes='confirm'))
			left.result, right.result = False, True
			if self.default: left, right = right, left
			yield left; yield right
			left.focus()
	def finish(self, result):
		self.result = result
		self.app.pop_screen()
		self.done.set()
	def on_button_pressed(self, message):
		self.finish(message.button.result)
	def action_exit_menu(self):
		self.finish(False)

class DiffEntry(ListItem):
	DEFAULT_CSS = '''
		DiffEntry .icon, DiffEntry .type { margin-right: 1; }
		DiffEntry .icon { width: 1; text-style: bold; color: black; }
		DiffEntry .type { color: $text-muted; }
		DiffEntry .side { width: 1fr; }
		DiffEntry .left { margin-right: 1; }
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
	def on_click(self, message):
		view = self.parent
		index = view._nodes.index(self)
		if index == view.index: return
		view.focus()
		view.index = index
		message.prevent_default()

class DirDiffApp(App):
	TITLE = 'DirDiff'
	BINDINGS = [
		BindingDesc('q', 'quit()', 'quit',
			long_description='quit the app'),
		BindingDesc('right', 'select()', 'select',
			key_display='▶', show=False,
			long_description='enter directory / open files in editor'),
		BindingDesc('left', 'leave()', 'leave',
			key_display='◀', show=True,
			long_description='leave the current directory'),
		BindingDesc('r', 'refresh()', 'refresh', show=False,
			long_description='refresh files and diffs'),
		BindingDesc('s', 'shell("left")', 'shell-left',
			key_display='s', show=False,
			long_description='open shell in the left directory'),
		BindingDesc('S', 'shell("right")', 'shell-right',
			key_display='S', show=False,
			long_description='open shell in the right directory'),
		BindingDesc('c', 'copy("right","left")', 'copy-left',
			key_display='c', show=False,
			long_description='copy right to left side'),
		BindingDesc('C', 'copy("left","right")', 'copy-right',
			key_display='C', show=False,
			long_description='copy left to right side'),
		BindingDesc('d', 'delete("left")', 'delete-left',
			key_display='d', show=False,
			long_description='delete the left file'),
		BindingDesc('D', 'delete("right")', 'delete-right',
			key_display='D', show=False,
			long_description='delete the right file'),
		BindingDesc('?', 'menu()', 'menu', key_display='?',
			long_description='toggle this menu'),
	]
	CSS = '''
		$background: #000;
		#paths { height: 1; overflow-y: scroll; }
		#paths Label { width: 1fr }
		#paths .left { margin-right: 1; }
		#files { background: $background; height: 1fr; overflow-y: scroll; }
		ListItem { background: #0000; }
		ListItem.--highlight { background: $accent 20%; }
		ListView:focus > ListItem.--highlight { background: $accent 33%; }
		ModalScreen { align: center middle; background: $background 50%; }
		ModalScreen > Widget {
			border: solid;
			min-width: 40; width: 50%; min-height: 4;
			max-width: 100%; height: 50%; max-height: 100%;
		}
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
		env = os.environ.copy()
		env['DDIFF_LEFT'] = str(self.conf.left.joinpath(self.cwd).resolve())
		env['DDIFF_RIGHT'] = str(self.conf.right.joinpath(self.cwd).resolve())
		with self.suspend():
			subprocess.run(os.environ.get('SHELL', 'sh'),
				shell=True, cwd=path, env=env,
				stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)
	def action_menu(self):
		self.push_screen(MenuScreen())
	def action_copy(self, source, target):
		name = self.query_one('#files').highlighted_child.name
		source = getattr(self.conf, source) / self.cwd / name
		target = getattr(self.conf, target) / self.cwd / name
		if not source.exists():
			return self.push_screen(MessageScreen('Copy',
				'Source does not exist: {}'.format(source)))
		if not target.exists():
			return self.do_copy(source, target)
		screen = ConfirmScreen('Copy',
			'Overwrite {} with {}'.format(target, source))
		self.push_screen(screen)
		async def task():
			await screen.done.wait()
			if not screen.result: return
			self.do_copy(source, target)
		asyncio.create_task(task())
	def do_copy(self, source, target):
		try:
			if source.is_dir():
				shutil.copytree(source, target)
			else:
				shutil.copy2(source, target)
		except Exception as exc:
			self.push_screen(MessageScreen('Copy', str(exc)))
		else:
			self.action_refresh()
	def action_delete(self, side):
		name = self.query_one('#files').highlighted_child.name
		path = getattr(self.conf, side) / self.cwd / name
		if not path.exists(): return
		screen = ConfirmScreen('Delete', 'Delete {}'.format(path))
		self.push_screen(screen)
		async def task():
			await screen.done.wait()
			if not screen.result: return
			if path.is_dir(): shutil.rmtree(path)
			else: path.unlink()
			self.action_refresh()
		asyncio.create_task(task())

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
