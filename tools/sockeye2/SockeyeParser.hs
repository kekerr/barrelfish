{-
  SockeyeParser.hs: Parser for Sockeye

  Part of Sockeye

  Copyright (c) 2017, ETH Zurich.

  All rights reserved.

  This file is distributed under the terms in the attached LICENSE file.
  If you do not find this file, copies can be found by writing to:
  ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
  Attn: Systems Group.
-}

module SockeyeParser
( parseSockeye ) where

import Control.Monad

import Text.ParserCombinators.Parsec as Parsec
import qualified Text.ParserCombinators.Parsec.Token as P
import Text.ParserCombinators.Parsec.Language (javaStyle)

import qualified SockeyeAST as AST

{- Setup the lexer -}
lexer = P.makeTokenParser (
    javaStyle  {
        {- list of reserved Names -}
        P.reservedNames = [
            "is", "are",
            "accept", "map",
            "over",
            "to", "at"
        ],

        {- valid identifiers -}
        P.identStart = letter,
        P.identLetter = alphaNum,

        {- comment start and end -}
        P.commentStart = "/*",
        P.commentEnd = "*/",
        P.commentLine = "//",
        P.nestedComments = False
    })

{- Helper functions -}
whiteSpace    = P.whiteSpace lexer
identifier    = P.identifier lexer <?> "node identifier"
reserved      = P.reserved lexer
address       = liftM fromIntegral (P.natural lexer) <?> "address"
brackets      = P.brackets lexer
symbol        = P.symbol lexer
stringLiteral = P.stringLiteral lexer
commaSep      = P.commaSep lexer
commaSep1     = P.commaSep1 lexer

{- Sockeye parsing -}
sockeyeFile = do
    whiteSpace
    nodes <- many netSpec
    eof
    return $ AST.NetSpec $ concat nodes

netSpec = do
    nodeIds <- try single <|> multiple
    node <- nodeSpec
    return $ map (\nodeId -> (nodeId, node)) nodeIds
    where single = do
            nodeId <- identifier
            reserved "is"
            return [nodeId]
          multiple = do
            nodeIds <- commaSep1 identifier
            reserved "are"
            return nodeIds

nodeSpec = do
    accept <- try parseAccept <|> return []
    translate <- try parseTranlsate <|> return []
    overlay <- try parseOverlay <|> return Nothing
    return $ AST.NodeSpec accept translate overlay
    where parseAccept = do
            reserved "accept"
            brackets $ commaSep blockSpec
          parseTranlsate = do
            reserved "map"
            brackets $ commaSep mapSpec
          parseOverlay = do
            reserved "over"
            nodeId <- identifier
            return (Just nodeId)

mapSpec = do
    srcBlock <- blockSpec
    reserved "to"
    destNode <- identifier
    reserved "at"
    destBase <- address
    return $ AST.MapSpec srcBlock destNode destBase

blockSpec = do
    base <- address
    limit <- try parseLimit <|> return base
    return $ AST.BlockSpec base limit
    where parseLimit = do
            symbol "-"
            address

parseSockeye :: String -> String -> Either ParseError AST.NetSpec
parseSockeye = parse sockeyeFile
