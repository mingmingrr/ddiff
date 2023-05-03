#!/usr/bin/env python3

from __future__ import annotations

from textual.app import *
from textual.widgets import *
from textual.containers import *
from textual.reactive import *
from textual.binding import *

import re
import os
import shlex
import argparse
import enum
import dataclasses
import subprocess
import filecmp
import natsort
from pathlib import Path

import rich.traceback
rich.traceback.install(show_locals=True)

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

class Status(enum.Enum):
	Matching  = enum.auto()
	Unknown   = enum.auto()
	Different = enum.auto()
	OneSided  = enum.auto()

@dataclasses.dataclass
class Entry:
	name:   str
	status: Status
	left:   FileType
	right:  FileType

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

class DirDiffItem(ListItem):
	DEFAULT_CSS = '''
		DirDiffList > DirDiffItem.--highlight { background: $accent 33%; }
		DirDiffList:focus > DirDiffItem.--highlight { background: $accent 33%; }
		DirDiffItem { background: #0000; }
		/* DirDiffItem .name { background: #000; } */
		DirDiffItem .type { color: $text-muted; margin-right: 1; }
		DirDiffItem .icon {
			width: 1;
			margin-right: 1;
			text-style: bold;
			color: #000;
		}
		DirDiffItem.different .icon { background: $warning; }
		DirDiffItem.onesided  .icon { background: $success; }
		DirDiffItem.missing   .icon { background: $error; }
		DirDiffItem.missing   .type { width: 0; }
		DirDiffItem.missing   .name { width: 0; }
		DirDiffItem.unknown   .icon { background: $secondary; }
	'''

class DirDiffList(ListView):
	other = var(None)
	def watch_index(self, old, new):
		super().watch_index(old, new)
		if self.other.index != new:
			self.other.index = new

class DirDiffApp(App):
	BINDINGS = [
		Binding('q', 'quit', 'quit'),
		Binding('enter,e,right', 'enter', 'enter', priority=True, key_display='▶'),
		Binding('escape,left', 'leave', 'leave', key_display='◀'),
		Binding('r', 'refresh', 'refresh'),
		Binding('s', 'shell(0)', 'shell-left', key_display='s'),
		Binding('S', 'shell(1)', 'shell-right', key_display='S'),
	]
	CSS = '''
		#panels {
			layout: grid;
			grid_size: 2 1;
			background: #000;
		}
		.panel {
			border: solid $accent;
		}
		.panel > Label {
			width: 100%;
		}
		.panel > DirDiffList {
			border-top: solid $accent;
		}
	'''

	conf = var(None)
	cwd = reactive(Path('.'), always_update=True)
	diff = reactive([])

	def __init__(self, *args, config=None, **kwargs):
		super().__init__(*args, **kwargs)
		assert config is not None
		self.conf = config
	def compose(self):
		yield Header()
		with Container(id='panels'):
			left, right = DirDiffList(), DirDiffList()
			left.other, right.other = right, left
			with Vertical(id='left', classes='panel'):
				yield Label('')
				yield left
			with Vertical(id='right', classes='panel'):
				yield Label('')
				yield right
		yield Footer()
	def on_mount(self):
		self.query_one('#left > ListView').focus()
	def watch_cwd(self, old, new):
		conf = self.conf
		left = conf.left / self.cwd
		right = conf.right / self.cwd
		self.query_one('#left > Label').update(str(left))
		self.query_one('#right > Label').update(str(right))
		self.diff = list(diff_dir(left, right, conf.exclude))
	def watch_diff(self, old, new):
		conf = self.conf
		lview = self.query_one('#left > ListView')
		rview = self.query_one('#right > ListView')
		lview.clear(); rview.clear()
		for entry in new:
			lview.append(self.make_item(entry, entry.left))
			rview.append(self.make_item(entry, entry.right))
	@staticmethod
	def make_item(entry, ftype):
		if ftype == FileType.Missing: status = 'missing'
		else: status = entry.status.name.lower()
		return DirDiffItem(Horizontal(
			Label(diff_icons[status], classes='icon'),
			Label(ftype.value, classes='type'),
			Label(entry.name, classes='name type-{}'.format(ftype.value)),
		), classes=status)
	def action_enter(self):
		index = self.query_one('#left > ListView').index
		entry = self.diff[index]
		path = self.cwd / entry.name
		left, right = self.conf.left / path, self.conf.right / path
		cmd = '{} {} {}'.format(self.conf.editor,
			shlex.quote(str(left)), shlex.quote(str(right)))
		if left.is_dir() and right.is_dir() and left.name == right.name:
			self.cwd = self.cwd / left.name
		else:
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
		trace('shell', index)

diff_icons = {
	'matching': ' ',
	'different': '*',
	'onesided': '+',
	'missing': '-',
	'unknown': '?',
}

def diff_dir(left, right, exclude):
	lefts = natsort.natsorted(left.iterdir(), reverse=True)
	rights = natsort.natsorted(right.iterdir(), reverse=True)
	yield from diff_files(lefts, rights, exclude)

def diff_files(lefts, rights, exclude):
	while lefts and rights:
		l, r = lefts[-1], rights[-1]
		lk, rk = natsort.natsort_key(l.name), natsort.natsort_key(r.name)
		if lk == rk and l.name == r.name:
			lefts.pop(), rights.pop()
			if exclude.match(l.name) is not None: continue
			yield Entry(l.name, *diff_file(l, r, exclude))
		elif lk < rk or (lk == rk and l.name < r.name):
			lefts.pop()
			if exclude.match(l.name) is not None: continue
			yield Entry(l.name, Status.OneSided,
				file_type(l), FileType.Missing)
		else:
			rights.pop()
			if exclude.match(r.name) is not None: continue
			yield Entry(r.name, Status.OneSided,
				FileType.Missing, file_type(r))
	for l in lefts:
		if exclude.match(l.name) is not None: continue
		yield Entry(l.name, Status.OneSided,
			file_type(l), FileType.Missing)
	for r in rights:
		if exclude.match(r.name) is not None: continue
		yield Entry(r.name, Status.OneSided,
			FileType.Missing, file_type(r))

def diff_file(left, right, exclude):
	lstat, rstat = left.stat(), right.stat()
	ltype, rtype = file_type(left), file_type(right)
	if lstat.st_dev == rstat.st_dev and lstat.st_ino == rstat.st_ino:
		return Status.Matching, ltype, rtype
	if left.is_symlink():
		status, left, right = diff_file(left.resolve(), right, exclude)
		return status, ltype, right
	if right.is_symlink():
		status, left, right = diff_file(left, right.resolve(), exclude)
		return status, left, rtype
	if ltype != rtype:
		return Status.Different, ltype, rtype
	if left.is_dir():
		status = Status.Matching
		for entry in diff_dir(left, right, exclude):
			if entry.status == Status.Matching:
				continue
			if entry.status == Status.Unknown:
				status = Status.Unknown
				continue
			return Status.Different, ltype, rtype
		return status, ltype, rtype
	if left.is_file():
		if filecmp.cmp(left, right):
			return Status.Matching, ltype, rtype
		return Status.Different, ltype, rtype
	return Status.Unknown, ltype, rtype

ansi_colors = [i.strip() for i in '''
	#000000
	#cc0000
	#00cc00
	#cccc00
	#0077cc
	#cc00dd
	#00cccc
	#cccccc
'''.strip().splitlines()]

ansi_bright_colors = [i.strip() for i in '''
	#777777
	#ff2222
	#22ff00
	#ffff00
	#22aaff
	#ff22dd
	#00ffff
	#ffffff
'''.strip().splitlines()]

ansi_styles = {
	'1': 'text-style: bold;',
	'3': 'text-style: italic;',
	'4': 'text-style: underline;',
	'7': 'text-style: reverse;',
	'9': 'text-style: strike;',
	**{str(k): 'color: {};'.format(v)
		for k, v in enumerate(ansi_colors, start=30)},
	**{str(k): 'color: {};'.format(v)
		for k, v in enumerate(ansi_bright_colors, start=90)},
	**{str(k): 'background: {};'.format(v)
		for k, v in enumerate(ansi_colors, start=40)},
	**{str(k): 'background: {};'.format(v)
		for k, v in enumerate(ansi_bright_colors, start=100)},
}

def main():
	trace('-' * 80)
	args = parser.parse_args()
	args.exclude = re.compile('|'.join(
		'(?:{})'.format(re.compile(i).pattern) for i in args.exclude))
	trace(args)
	colors = {
		'lc':'\x1b',     'rc':'m',        'rs':'0',        'cl':'\x1b[K',
		'rs':'0',        'di':'01;34',    'ln':'01;36',    'mh':'00',
		'pi':'40;33',    'so':'01;35',    'bd':'40;33;01', 'do':'01;35',
		'cd':'40;33;01', 'or':'40;31;01', 'mi':'00',       'su':'37;41',
		'sg':'30;43',    'ca':'00',       'tw':'30;42',    'ow':'34;42',
		'st':'37;44',    'ex':'01;32' }
	colors.update(x.split('=', maxsplit=1) for x in
		os.environ.get('LS_COLORS', '').split(':') if x)
	for ftype in FileType:
		DirDiffApp.CSS += '\nDirDiffItem Label.type-{} {{ {} }}'.format(ftype.value,
			''.join(ansi_styles.get(style.lstrip('0'), '')
				for style in colors.get(ftype.value, '').split(';')))
	DirDiffApp(config=args).run()

if __name__ == '__main__': main()
