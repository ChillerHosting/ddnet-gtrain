#include "connection.h"

#include <base/str.h>

IDbConnection::IDbConnection(const char *pPrefix)
{
	str_copy(m_aPrefix, pPrefix);
}

std::optional<float> IDbConnection::GetOptionalFloat(int Col)
{
	if(IsNull(Col))
		return std::nullopt;
	return GetFloat(Col);
}

std::optional<int> IDbConnection::GetOptionalInt(int Col)
{
	if(IsNull(Col))
		return std::nullopt;
	return GetInt(Col);
}

std::optional<int64_t> IDbConnection::GetOptionalInt64(int Col)
{
	if(IsNull(Col))
		return std::nullopt;
	return GetInt64(Col);
}

void IDbConnection::FormatCreateRace(char *aBuf, unsigned int BufferSize, bool Backup) const
{
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_race%s ("
		"  Map VARCHAR(128) COLLATE %s NOT NULL, "
		"  Name VARCHAR(%d) COLLATE %s NOT NULL, "
		"  Timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
		"  Time FLOAT DEFAULT 0, "
		"  Server CHAR(4), "
		"  cp1 FLOAT DEFAULT 0, cp2 FLOAT DEFAULT 0, cp3 FLOAT DEFAULT 0, "
		"  cp4 FLOAT DEFAULT 0, cp5 FLOAT DEFAULT 0, cp6 FLOAT DEFAULT 0, "
		"  cp7 FLOAT DEFAULT 0, cp8 FLOAT DEFAULT 0, cp9 FLOAT DEFAULT 0, "
		"  cp10 FLOAT DEFAULT 0, cp11 FLOAT DEFAULT 0, cp12 FLOAT DEFAULT 0, "
		"  cp13 FLOAT DEFAULT 0, cp14 FLOAT DEFAULT 0, cp15 FLOAT DEFAULT 0, "
		"  cp16 FLOAT DEFAULT 0, cp17 FLOAT DEFAULT 0, cp18 FLOAT DEFAULT 0, "
		"  cp19 FLOAT DEFAULT 0, cp20 FLOAT DEFAULT 0, cp21 FLOAT DEFAULT 0, "
		"  cp22 FLOAT DEFAULT 0, cp23 FLOAT DEFAULT 0, cp24 FLOAT DEFAULT 0, "
		"  cp25 FLOAT DEFAULT 0, "
		"  GameId VARCHAR(64), "
		"  DDNet7 BOOL DEFAULT FALSE, "
		"  PRIMARY KEY (Map, Name, Time, Timestamp, Server)"
		")",
		GetPrefix(), Backup ? "_backup" : "",
		BinaryCollate(), MAX_NAME_LENGTH_SQL, BinaryCollate());
}

void IDbConnection::FormatCreateTeamrace(char *aBuf, unsigned int BufferSize, const char *pIdType, bool Backup) const
{
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_teamrace%s ("
		"  Map VARCHAR(128) COLLATE %s NOT NULL, "
		"  Name VARCHAR(%d) COLLATE %s NOT NULL, "
		"  Timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
		"  Time FLOAT DEFAULT 0, "
		"  ID %s NOT NULL, " // VARBINARY(16) for MySQL and BLOB for SQLite
		"  GameId VARCHAR(64), "
		"  DDNet7 BOOL DEFAULT FALSE, "
		"  PRIMARY KEY (Id, Name)"
		")",
		GetPrefix(), Backup ? "_backup" : "",
		BinaryCollate(), MAX_NAME_LENGTH_SQL, BinaryCollate(), pIdType);
}

void IDbConnection::FormatCreateMaps(char *aBuf, unsigned int BufferSize) const
{
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_maps ("
		"  Map VARCHAR(128) COLLATE %s NOT NULL, "
		"  Server VARCHAR(32) COLLATE %s NOT NULL, "
		"  Mapper VARCHAR(128) COLLATE %s NOT NULL, "
		"  Points INT DEFAULT 0, "
		"  Stars INT DEFAULT 0, "
		"  Timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
		"  PRIMARY KEY (Map)"
		")",
		GetPrefix(), BinaryCollate(), BinaryCollate(), BinaryCollate());
}

void IDbConnection::FormatCreateSaves(char *aBuf, unsigned int BufferSize, bool Backup) const
{
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_saves%s ("
		"  Savegame TEXT COLLATE %s NOT NULL, "
		"  Map VARCHAR(128) COLLATE %s NOT NULL, "
		"  Code VARCHAR(128) COLLATE %s NOT NULL, "
		"  Timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
		"  Server CHAR(4), "
		"  DDNet7 BOOL DEFAULT FALSE, "
		"  SaveId VARCHAR(36) DEFAULT NULL, "
		"  PRIMARY KEY (Map, Code)"
		")",
		GetPrefix(), Backup ? "_backup" : "",
		BinaryCollate(), BinaryCollate(), BinaryCollate());
}

void IDbConnection::FormatCreatePoints(char *aBuf, unsigned int BufferSize) const
{
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_points ("
		"  Name VARCHAR(%d) COLLATE %s NOT NULL, "
		"  Points INT DEFAULT 0, "
		"  PRIMARY KEY (Name)"
		")",
		GetPrefix(), MAX_NAME_LENGTH_SQL, BinaryCollate());
}

void IDbConnection::FormatCreateGTrainStats(char *aBuf, unsigned int BufferSize) const
{
	// GTrain totals per player name, they only ever grow. Playtime is in
	// seconds.
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_gtrain_stats ("
		"  Name VARCHAR(%d) COLLATE %s NOT NULL, "
		"  Points BIGINT DEFAULT 0, "
		"  Captures INT DEFAULT 0, "
		"  Attempts INT DEFAULT 0, "
		"  Respawns INT DEFAULT 0, "
		"  Playtime INT DEFAULT 0, "
		"  PRIMARY KEY (Name)"
		")",
		GetPrefix(), MAX_NAME_LENGTH_SQL, BinaryCollate());
}

void IDbConnection::FormatCreateGTrainRuns(char *aBuf, unsigned int BufferSize, const char *pBlobType) const
{
	// Every GTrain capture made in control, as data that replays it. The
	// map's hash and the server's git revision tell the map and physics it
	// was made with. A run starts at a tick of its game, the server's map
	// load, and lasts Ticks. Elo is what the capture added to the player's
	// ELO, SpeedFactor the multiplier its speed gave the points and Points
	// the points it added, NULL on runs stored before they were. Banned runs
	// are shadow-banned (ban_runs): they stay stored, but what they earned is
	// taken off the player's stats.
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_gtrain_runs ("
		"  Map VARCHAR(128) COLLATE %s NOT NULL, "
		"  MapSha256 CHAR(64) NOT NULL, "
		"  Name VARCHAR(%d) COLLATE %s NOT NULL, "
		"  Timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
		"  GameId VARCHAR(64) NOT NULL, "
		"  Revision VARCHAR(64) NOT NULL, "
		"  StartTick INT NOT NULL, "
		"  Ticks INT NOT NULL, "
		"  Distance INT NOT NULL, "
		"  Elo FLOAT NOT NULL, "
		"  SpeedFactor FLOAT NOT NULL, "
		"  Run %s NOT NULL, "
		"  Points BIGINT DEFAULT NULL, "
		"  Banned BOOL NOT NULL DEFAULT FALSE, "
		"  PRIMARY KEY (Map, GameId, StartTick, Name)"
		")",
		GetPrefix(), BinaryCollate(), MAX_NAME_LENGTH_SQL, BinaryCollate(), pBlobType);
}
