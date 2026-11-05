#include "core/archive_internal.h"
#include "core/archive_tar.h"
#include "core/compression.h"
#include "core/md5.h"
#include <gtest/gtest.h>
#include <zlib.h>
#include <random>

TEST(PipelineAlgorithms, Md5Vectors) {
 EXPECT_EQ(dbackup::crypto::md5(nullptr,0),"d41d8cd98f00b204e9800998ecf8427e");
 const std::string text="abc";
 EXPECT_EQ(dbackup::crypto::md5(reinterpret_cast<const uint8_t*>(text.data()),text.size()),"900150983cd24fb0d6963f7d28e17f72");
}
TEST(PipelineAlgorithms, KnownHuffmanAndLz77Vectors) {
 std::string error;std::vector<uint8_t> input{'a','a','a','b'},encoded,decoded;
 ASSERT_TRUE(dbackup::compression::encode(Compression::Huffman,input,encoded,error));
 ASSERT_EQ(encoded.size(),1025u);EXPECT_EQ(encoded[97*4],3);EXPECT_EQ(encoded[98*4],1);EXPECT_EQ(encoded.back(),7);
 ASSERT_TRUE(dbackup::compression::decode(Compression::Huffman,encoded,input.size(),decoded,error));EXPECT_EQ(decoded,input);
 input={'a','b','a','b','a','b','a','b'};
 ASSERT_TRUE(dbackup::compression::encode(Compression::Lz77,input,encoded,error));
 EXPECT_EQ(encoded,(std::vector<uint8_t>{0,'a',0,'b',1,2,0,3}));
 ASSERT_TRUE(dbackup::compression::decode(Compression::Lz77,encoded,input.size(),decoded,error));EXPECT_EQ(decoded,input);
 EXPECT_FALSE(dbackup::compression::decode(Compression::Lz77,{1,0,0,0},3,decoded,error));
}
TEST(PipelineAlgorithms, CompressionAndCorruption) {
 std::mt19937 gen(42);std::vector<std::vector<uint8_t>> inputs{{},{'A'},std::vector<uint8_t>(10000,'x'),std::vector<uint8_t>(9000)};
 for(auto& b:inputs.back())b=gen();
 for(auto method:{Compression::None,Compression::Huffman,Compression::Lz77,Compression::Deflate})for(const auto& input:inputs){
  std::vector<uint8_t> encoded,decoded;std::string error;
  ASSERT_TRUE(dbackup::compression::encode(method,input,encoded,error))<<error;
  ASSERT_TRUE(dbackup::compression::decode(method,encoded,input.size(),decoded,error))<<error;EXPECT_EQ(input,decoded);
  if(!encoded.empty()){encoded.pop_back();EXPECT_FALSE(dbackup::compression::decode(method,encoded,input.size(),decoded,error));}
 }
}
TEST(PipelineAlgorithms, DeflateCrossChecksZlib) {
 std::vector<uint8_t> input(4096,'q'),encoded,decoded;std::string error;
 ASSERT_TRUE(dbackup::compression::encode(Compression::Deflate,input,encoded,error));
 decoded.resize(input.size());uLongf n=decoded.size();ASSERT_EQ(uncompress(decoded.data(),&n,encoded.data(),encoded.size()),Z_OK);EXPECT_EQ(decoded,input);
 uLongf size=compressBound(input.size());encoded.resize(size);ASSERT_EQ(compress2(encoded.data(),&size,input.data(),input.size(),9),Z_OK);encoded.resize(size);
 ASSERT_TRUE(dbackup::compression::decode(Compression::Deflate,encoded,input.size(),decoded,error))<<error;EXPECT_EQ(decoded,input);
 encoded.push_back(0);EXPECT_FALSE(dbackup::compression::decode(Compression::Deflate,encoded,input.size(),decoded,error));
}
TEST(PipelineAlgorithms, AllFormatCombinations) {
 using dbackup::archive_format::Entry;Entry e;e.file.relativePath="long/path/file.txt";e.file.metadata.mode=0100640;e.file.metadata.uid=1000;e.file.metadata.gid=1001;e.file.metadata.mtimeNs=1700000000123456789LL;e.data.assign(5000,'a');
 for(auto format:{PackFormat::Custom,PackFormat::Tar})for(auto comp:{Compression::None,Compression::Huffman,Compression::Lz77,Compression::Deflate})for(bool encrypted:{false,true}){
  PackOptions options;options.format=format;options.compression=comp;options.password=encrypted?"test-password":"";
  std::string error;std::vector<uint8_t> encoded;std::vector<Entry> decoded;
  ASSERT_TRUE(dbackup::archive_format::encodeCurrent({e},options,encoded,error))<<error;
  if(encrypted)EXPECT_FALSE(dbackup::archive_format::decodeCurrent(encoded,"wrong",decoded,error));
  ASSERT_TRUE(dbackup::archive_format::decodeCurrent(encoded,options.password,decoded,error))<<error;
  ASSERT_EQ(decoded.size(),1u);EXPECT_EQ(decoded[0].data,e.data);EXPECT_EQ(decoded[0].file.relativePath,e.file.relativePath);EXPECT_EQ(decoded[0].file.metadata.mtimeNs,e.file.metadata.mtimeNs);
  if(format!=PackFormat::Tar||comp!=Compression::None||encrypted){encoded[encoded.size()/2]^=1;EXPECT_FALSE(dbackup::archive_format::decodeCurrent(encoded,options.password,decoded,error));}
 }
}
