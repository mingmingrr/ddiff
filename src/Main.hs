-- {-# LANGUAGE LambdaCase #-}
-- {-# LANGUAGE TupleSections #-}
{-# LANGUAGE OverloadedStrings #-}
-- {-# LANGUAGE MultiWayIf #-}
{-# LANGUAGE ViewPatterns #-}
{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE RecordWildCards #-}

module Main where

import Debug.Trace

import qualified Algorithms.NaturalSort as NaturalSort

import Brick ((<+>), (<=>))
import qualified Brick as B
import qualified Brick.Widgets.List as B
import qualified Brick.Widgets.Border as B
import qualified Brick.Widgets.Center as B

import qualified Graphics.Vty as V

import Control.Monad
import Control.Monad.IO.Class
import Control.Exception

import System.Environment
import System.Process
import System.FilePath
import System.Directory hiding (isSymbolicLink)
import System.PosixCompat

import Data.Foldable
import Data.Char
import Data.Maybe
import Data.List
import Data.Bool
import Data.Functor
import qualified Data.Text as Text
import qualified Data.ByteString as BS

import qualified Options.Applicative as Opt

data Options = Options
  { optionEditor :: String
  , optionDir1 :: FilePath
  , optionDir2 :: FilePath
  } deriving (Show)

optionsParser :: Opt.ParserInfo Options
optionsParser = Opt.info (Opt.helper <*> prog) desc where
  prog = Options
    <$> Opt.strOption
        ( Opt.short 'e'
       <> Opt.long "editor"
       <> Opt.value "$EDITOR -d"
       <> Opt.showDefault
       <> Opt.help "Program used to edit two files"
        )
    <*> Opt.strArgument (Opt.metavar "DIR1")
    <*> Opt.strArgument (Opt.metavar "DIR2")
  desc = Opt.fullDesc <> Opt.progDesc "diff two directories"

data FileType
  = Missing
  | RegularFile
  | Directory
  | Symlink
  | BlockDevice
  | CharDevice
  | NamedPipe
  | Socket
  deriving (Eq, Show)

data DiffStatus
  = Matching
  | Unknown
  | Different
  | OneSided
  deriving (Eq, Show)

data DiffInfo = DiffInfo
  { diffName :: String
  , diffStatus :: DiffStatus
  , diffLeft :: FileType
  , diffRight :: FileType
  } deriving (Show)

fileType :: FileStatus -> FileType
fileType (isRegularFile     -> True) = RegularFile
fileType (isDirectory       -> True) = Directory
fileType (isSymbolicLink    -> True) = Symlink
fileType (isBlockDevice     -> True) = BlockDevice
fileType (isCharacterDevice -> True) = CharDevice
fileType (isNamedPipe       -> True) = NamedPipe
fileType (isSocket          -> True) = Socket
fileType _                           = BlockDevice

diffFolders :: FilePath -> FilePath -> IO [DiffInfo]
diffFolders x y = sequence =<< diffFolders' x y

diffFolders' :: FilePath -> FilePath -> IO [IO DiffInfo]
diffFolders' x y = liftM2 diffFiles
  (map (x </>) . sortOn NaturalSort.sortKey <$> listDirectory x)
  (map (y </>) . sortOn NaturalSort.sortKey <$> listDirectory y)

diffFiles :: [FilePath] -> [FilePath] -> [IO DiffInfo]
diffFiles xs ys = case (xs, ys) of
  (xs, []) -> map left xs
  ([], ys) -> map right xs
  (x:xs, y:ys) -> case NaturalSort.compare (takeFileName x) (takeFileName y) of
    LT -> left x : diffFiles xs (y:ys)
    GT -> right y : diffFiles (x:xs) ys
    EQ -> case compare (takeFileName x) (takeFileName y) of
      LT -> left x : diffFiles xs (y:ys)
      GT -> right y : diffFiles (x:xs) ys
      EQ -> diffFile x y : diffFiles xs ys
  where left x = getFileStatus x <&> \t ->
          DiffInfo (takeFileName x) OneSided (fileType t) Missing
        right x = getFileStatus x <&> \t ->
          DiffInfo (takeFileName x) OneSided Missing (fileType t)

diffFile :: FilePath -> FilePath -> IO DiffInfo
diffFile x y = do
  xs <- getFileStatus x
  ys <- getFileStatus y
  let status s = DiffInfo (takeFileName x) s (fileType xs) (fileType ys)
  case (fileType xs, fileType ys) of
    _ | (deviceID xs, fileID xs) == (deviceID ys, fileID ys) ->
      return (status Matching)
    (Symlink, _) -> getSymbolicLinkTarget x >>= \x' ->
      diffFile x' y <&> \info -> info{diffName=x, diffLeft=Symlink}
    (_, Symlink) -> getSymbolicLinkTarget y >>= \y' ->
      diffFile x y' <&> \info -> info{diffRight=Symlink}
    (xt, yt) | xt /= yt ->
      return (status Different)
    (Directory, Directory) ->
      let match True [] = return Unknown
          match False [] = return Matching
          match unknown (x:xs) = x >>= \d -> case diffStatus d of
            Matching -> match unknown xs
            Unknown -> match True xs
            _ -> return Different
       in fmap status . match False =<< diffFolders' x y
    (RegularFile, RegularFile)
      | fileSize xs /= fileSize ys ->
        return (status Different)
      | otherwise ->
        let handler (_ :: IOException) = return (status Unknown)
            action = liftM2 (==) (BS.readFile x) (BS.readFile y) <&> 
              bool (status Different) (status Matching)
         in action `catch` handler
    _ -> return (status Unknown)

data AppState = AppState
  { stateOpts :: Options
  , stateBase1 :: FilePath
  , stateBase2 :: FilePath
  , stateCwd :: [String]
  , stateDiff :: [DiffInfo]
  , stateIndex :: Int
  } deriving (Show)

type App = B.App AppState () ()

app :: App
app = B.App
  { B.appDraw         = appDraw
  , B.appChooseCursor = appChooseCursor
  , B.appHandleEvent  = appHandleEvent
  , B.appStartEvent   = appStartEvent
  , B.appAttrMap      = appAttrMap
  }

appChooseCursor :: AppState -> [B.CursorLocation n] -> Maybe (B.CursorLocation n)
appChooseCursor s xs = Nothing

appHandleEvent :: AppState -> B.BrickEvent n e -> B.EventM n (B.Next AppState)
appHandleEvent s (B.VtyEvent (V.EvKey key mod)) = case (key, mod) of
  (V.KChar 'q', []) -> B.halt s
  (V.KEsc, []) -> B.halt s
  (V.KUp, []) -> B.continue
    s{stateIndex=max 0 (stateIndex s - 1)}
  (V.KDown, []) -> B.continue
    s{stateIndex=min (length (stateDiff s)) (stateIndex s + 1)}
  (V.KHome, []) -> B.continue
    s{stateIndex=0}
  (V.KEnd,[]) -> B.continue
    s{stateIndex=length (stateDiff s)}
  (V.KPageUp, []) -> B.continue
    s{stateIndex=max 0 (stateIndex s - 20)}
  (V.KPageDown, []) -> B.continue
    s{stateIndex=min (length (stateDiff s)) (stateIndex s + 20)}
  (V.KEnter, []) -> enterEntry s
  (V.KRight, []) -> enterEntry s
  (V.KLeft, []) -> enterDirectory ".." s
  _ -> B.continue s
appHandleEvent s _ = B.continue s

enterEntry :: AppState -> B.EventM n (B.Next AppState)
enterEntry state@(AppState _ _ _ _ _ 0) = enterDirectory ".." state
enterEntry state@(AppState _ _ _ _ diff idx) = case (left, right) of
  (Directory, Directory) -> enterDirectory name state
  (Missing, Directory) -> B.continue state
  (Directory, Missing) -> B.continue state
  _ -> enterFile name state
  where DiffInfo name status left right = diff !! (idx - 1)

currentDirs :: AppState -> (FilePath, FilePath)
currentDirs (AppState _ base1 base2 cwd _ _) = (base1 </> cwd', base2 </> cwd')
  where cwd' = joinPath (reverse cwd)

updateDiff :: AppState -> IO AppState
updateDiff state@(AppState _ _ _ _ _ idx) = do
  ds <- liftIO (uncurry diffFolders (currentDirs state))
  return state{ stateDiff=ds, stateIndex=min idx (length ds) }

enterDirectory :: String -> AppState -> B.EventM n (B.Next AppState)
enterDirectory ".." state@(AppState _ _ _ [] _ _) = B.continue state
enterDirectory name state@(AppState _ base1 base2 cwd _ idx) =
  B.continue =<< liftIO (updateDiff state{stateCwd=cwd'})
  where cwd' = if name == ".." then tail cwd else name : cwd

enterFile :: String -> AppState -> B.EventM n (B.Next AppState)
enterFile name state = B.suspendAndResume $ do
  let (dir1, dir2) = currentDirs state
      cmd:args = words (optionEditor (stateOpts state))
  cmd' <- case cmd of
    '$':var -> fromMaybe "vim" <$> lookupEnv var
    _ -> return cmd
  callProcess cmd' (args ++ [dir1 </> name, dir2 </> name])
  updateDiff state

appStartEvent :: AppState -> B.EventM n AppState
appStartEvent = liftIO . updateDiff

appAttrMap :: AppState -> B.AttrMap
appAttrMap s = B.attrMap V.defAttr
  [ ("status-matching"      , V.defAttr)
  , ("status-missing"       , B.fg V.brightRed)
  , ("status-onesided"      , B.fg V.brightGreen)
  , ("status-different"     , B.fg V.brightYellow)
  , ("status-unknown"       , B.fg V.brightBlue)
  , ("filetype-regularfile" , V.defAttr)
  , ("filetype-directory"   , B.fg V.brightBlue)
  , ("filetype-symlink"     , B.fg V.brightCyan)
  , ("filetype-blockdevice" , B.fg V.brightYellow)
  , ("filetype-chardevice"  , B.fg V.brightYellow)
  , ("filetype-namedpipe"   , B.fg V.brightRed)
  , ("filetype-socket"      , B.fg V.brightMagenta)
  , ("selected"             , V.black `B.on` V.white) ]

appDraw :: AppState -> [B.Widget n]
appDraw state@(AppState _ base1 base2 _ diff _) =
  [ B.joinBorders $ B.vBox [ B.hBox [left, B.vBorder, right], B.hBorder, help ] ] where
    left = drawDiff base1 diffLeft state
    right = drawDiff base2 diffRight state
    help = B.strWrap "q/esc: quit. up/down: select. enter: edit/open"

drawDiff :: String -> (DiffInfo -> FileType) -> AppState -> B.Widget n
drawDiff base ftype state@(AppState _ _ _ cwd diff idx) = B.padRight B.Max . B.vBox
  $ drawLine False base status Directory
  : B.hBorder
  : drawLine (idx == 0) ".." Matching Directory
  : [ drawLine (idx == i) name status (ftype d)
    | (i, d@(DiffInfo name status _ _)) <- zip [1..] diff ]
  where
    status = if and [diffStatus d == Matching | d <- diff]
      then Matching else Different

drawLine :: Bool -> String -> DiffStatus -> FileType -> B.Widget n
drawLine selected name status ftype = B.hBox
  [ B.padRight (B.Pad 1) (drawSelect selected)
  , B.padRight (B.Pad 1) (drawStatus status ftype)
  , drawName name ftype ]

drawSelect :: Bool -> B.Widget n
drawSelect = bool (B.str " ")
  (B.withAttr (B.attrName "selected") (B.str " "))

drawStatus :: DiffStatus -> FileType -> B.Widget n
drawStatus status ftype = B.withAttr
  (B.attrName ("status-" ++ name)) (B.str [statusChar status ftype])
  where name = if ftype == Missing then "missing" else map toLower (show status)

drawName :: String -> FileType -> B.Widget n
drawName _ Missing = B.emptyWidget
drawName name ftype = B.withAttr
  (B.attrName ("filetype-" ++ map toLower (show ftype)))
  (B.str name)

statusChar :: DiffStatus -> FileType -> Char
statusChar Matching  _ = ' '
statusChar Different _ = '~'
statusChar Unknown   _ = '?'
statusChar OneSided Missing = '-'
statusChar OneSided _ = '+'

main :: IO ()
main = do
  opts@Options{..} <- Opt.execParser optionsParser
  let initialState = AppState opts optionDir1 optionDir2 [] [] 0
  void (B.defaultMain app initialState)

