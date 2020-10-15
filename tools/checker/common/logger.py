# Copyright (C) 2014 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import collections
import enum
import sys


class Logger:

  class Level(enum.IntEnum):
    NO_OUTPUT = 0
    ERROR = 1
    INFO = 2

  class Color(enum.Enum):
    DEFAULT = 0
    BLUE = 1
    GRAY = 2
    PURPLE = 3
    RED = 4
    GREEN = 5

    @staticmethod
    def terminalCode(color, out=sys.stdout):
      if not out.isatty():
        return ''
      elif color == Logger.Color.BLUE:
        return '\033[94m'
      elif color == Logger.Color.GRAY:
        return '\033[37m'
      elif color == Logger.Color.PURPLE:
        return '\033[95m'
      elif color == Logger.Color.RED:
        return '\033[91m'
      elif color == Logger.Color.GREEN:
        return '\033[32m'
      else:
        return '\033[0m'

  Verbosity = Level.INFO

  @staticmethod
  def log(content, level=Level.INFO, color=Color.DEFAULT, newLine=True, out=sys.stdout):
    if level <= Logger.Verbosity:
      content = Logger.Color.terminalCode(color, out) + str(content) + \
             Logger.Color.terminalCode(Logger.Color.DEFAULT, out)
      if newLine:
        print(content, file=out)
      else:
        print(content, end="", file=out)
      out.flush()

  @staticmethod
  def fail(msg, file=None, line=-1, lineText=None, variables=None):
    Logger.log("error: ", Logger.Level.ERROR, color=Logger.Color.RED, newLine=False, out=sys.stderr)
    Logger.log(msg, Logger.Level.ERROR, out=sys.stderr)

    if lineText:
      loc = ""
      if file:
        loc += file + ":"
      if line > 0:
        loc += str(line) + ":"
      if loc:
        loc += " "
      Logger.log(loc, Logger.Level.ERROR, color=Logger.Color.GRAY, newLine=False, out=sys.stderr)
      Logger.log(lineText, Logger.Level.ERROR, out=sys.stderr)

    if variables:
      longestName = 0
      for var in variables:
        longestName = max(longestName, len(var))

      for var in collections.OrderedDict(sorted(variables.items())):
        padding = ' ' * (longestName - len(var))
        Logger.log(var, Logger.Level.ERROR, color=Logger.Color.GREEN, newLine=False, out=sys.stderr)
        Logger.log(padding, Logger.Level.ERROR, newLine=False, out=sys.stderr)
        Logger.log(" = ", Logger.Level.ERROR, newLine=False, out=sys.stderr)
        Logger.log(variables[var], Logger.Level.ERROR, out=sys.stderr)

    sys.exit(1)

  @staticmethod
  def startTest(name):
    Logger.log("TEST ", color=Logger.Color.PURPLE, newLine=False)
    Logger.log(name + "... ", newLine=False)

  @staticmethod
  def testPassed():
    Logger.log("PASS", color=Logger.Color.BLUE)

  @staticmethod
  def testFailed(msg, statement, variables):
    Logger.log("FAIL", color=Logger.Color.RED)
    Logger.fail(msg, statement.fileName, statement.lineNo, statement.originalText, variables)
