{-
  SockeyeMain.hs: Sockeye

  Copyright (c) 2017, ETH Zurich.

  All rights reserved.

  This file is distributed under the terms in the attached LICENSE file.
  If you do not find this file, copies can be found by writing to:
  ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
  Attn: Systems Group.
-}

module Main where

import Control.Monad
import System.Console.GetOpt
import System.Exit
import System.Environment
import System.IO

import SockeyeAST as AST
import SockeyeParser

{- Possible options for the Sockeye Compiler -}
data Options = Options { optInputFile :: FilePath }

{- Default options -}
defaultOptions :: Options
defaultOptions = Options { optInputFile = ""}

{- Set the input file name -}
optSetInputFileName :: FilePath -> Options -> Options
optSetInputFileName f o = o { optInputFile = f }

{- Prints usage information possibly with usage errors -}
usage :: [String] -> IO ()
usage errors = do
    prg <- getProgName
    let usageString = "Usage: " ++ prg ++ " [options] file\nOptions:"
        in hPutStrLn stderr (usageInfo (concat errors ++ usageString) options)

{- Setup option parser -}
options :: [OptDescr (Options -> IO Options)]
options = 
    [ Option "h" ["help"]
        (NoArg (\_ -> do
                    usage []
                    exitWith ExitSuccess))
        "Show help"
    ]

{- evaluates the compiler options -}
compilerOpts :: [String] -> IO (Options)
compilerOpts argv =
    case getOpt Permute options argv of
        (actions, [f], []) -> liftM (optSetInputFileName f) $ foldl (>>=) (return defaultOptions) actions
        (actions, [], [])  -> usage ["No input file\n"] >> exitWith (ExitFailure 1)
        (_, _, errors)     -> usage errors >> exitWith (ExitFailure 1)

{- Runs the parser -}
parseFile :: FilePath -> IO (AST.NetSpec)
parseFile file = do
    src <- readFile file
    case parseSockeye file src of
        Left err -> hPutStrLn stderr ("Parse error at " ++ show err) >> exitWith (ExitFailure 2)
        Right ast -> return ast

main = do
    args <- getArgs
    opts <- compilerOpts args
    let inFile = optInputFile opts
    ast <- parseFile inFile
    print ast
    
