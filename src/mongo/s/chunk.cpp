/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk.h"

#include "mongo/platform/random.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Test whether we should split once data * splitTestFactor > chunkSize (approximately)
const int splitTestFactor = 5;

/**
 * Generates a random value for _dataWritten so that a mongos restart wouldn't cause delay in
 * splitting.
 */
int64_t mkDataWritten() {
    PseudoRandom r(static_cast<int64_t>(time(0)));
    return r.nextInt32(grid.getBalancerConfiguration()->getMaxChunkSizeBytes() / splitTestFactor);
}

}  // namespace

Chunk::Chunk(ChunkManager* manager, const ChunkType& from)
    : _manager(manager),
      _min(from.getMin().getOwned()),
      _max(from.getMax().getOwned()),
      _shardId(from.getShard()),
      _lastmod(from.getVersion()),
      _jumbo(from.getJumbo()),
      _dataWritten(mkDataWritten()) {
    invariantOK(from.validate());
}

Chunk::Chunk(ChunkManager* manager,
             const BSONObj& min,
             const BSONObj& max,
             const ShardId& shardId,
             ChunkVersion lastmod,
             uint64_t initialDataWritten)
    : _manager(manager),
      _min(min),
      _max(max),
      _shardId(shardId),
      _lastmod(lastmod),
      _jumbo(false),
      _dataWritten(initialDataWritten) {}

bool Chunk::containsKey(const BSONObj& shardKey) const {
    return getMin().woCompare(shardKey) <= 0 && shardKey.woCompare(getMax()) < 0;
}

uint64_t Chunk::getBytesWritten() const {
    return _dataWritten;
}

uint64_t Chunk::addBytesWritten(uint64_t bytesWrittenIncrement) {
    _dataWritten += bytesWrittenIncrement;
    return _dataWritten;
}

void Chunk::clearBytesWritten() {
    _dataWritten = 0;
}

void Chunk::randomizeBytesWritten() {
    _dataWritten = mkDataWritten();
}

std::string Chunk::toString() const {
    return str::stream() << ChunkType::shard() << ": " << _shardId << ", "
                         << ChunkType::DEPRECATED_lastmod() << ": " << _lastmod.toString() << ", "
                         << ChunkType::min() << ": " << _min << ", " << ChunkType::max() << ": "
                         << _max;
}

void Chunk::markAsJumbo(OperationContext* txn) const {
    log() << "Marking chunk " << toString() << " as jumbo.";

    // set this first
    // even if we can't set it in the db
    // at least this mongos won't try and keep moving
    _jumbo = true;

    const std::string chunkName = ChunkType::genID(_manager->getns(), _min);

    auto status = Grid::get(txn)->catalogClient(txn)->updateConfigDocument(
        txn,
        ChunkType::ConfigNS,
        BSON(ChunkType::name(chunkName)),
        BSON("$set" << BSON(ChunkType::jumbo(true))),
        false,
        ShardingCatalogClient::kMajorityWriteConcern);
    if (!status.isOK()) {
        warning() << "couldn't set jumbo for chunk: " << chunkName << causedBy(status.getStatus());
    }
}

}  // namespace mongo
