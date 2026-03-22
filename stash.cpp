// godot/modules/multiplayer/scene_replication_interface.cpp


/*
*	Holds encoded/compressed variants and slices them on demand
*
*	Slice returned from consume_slice_with_id_and_part only valid before calling non-const methods
*/
class BufferSlicer {
public:
	using ID = uint32_t;
	using Part = uint32_t;
	using Size = PackedByteArray::Size;

	static constexpr Size META_SIZE = sizeof(ID) + sizeof(Part);

	Error compress_and_store(const Vector<const Variant *> &p_var_ptrs, ID net_id) {
		int out_size = 0;
		Error err = MultiplayerAPI::encode_and_compress_variants(
				p_var_ptrs.ptr(), p_var_ptrs.size(), nullptr, out_size);
		ERR_FAIL_COND_V(err != OK, err);

		make_room(storage, Size(out_size) + META_SIZE);

		err = MultiplayerAPI::encode_and_compress_variants(
				p_var_ptrs.ptr(), p_var_ptrs.size(), storage.ptrw() + META_SIZE, out_size);
		ERR_FAIL_COND_V(err != OK, err);

		size = out_size;
		offset = META_SIZE;
		id = net_id;
		part_number = 0;
		return OK;
	}

	bool has_more() const {
		return offset < META_SIZE + size;
	}

	Span<const uint8_t> consume_slice_with_id_and_part(Size p_max_slice_size_with_meta) {
		ERR_FAIL_COND_V(p_max_slice_size_with_meta <= META_SIZE, {});
		if (!has_more()) {
			return {};
		}
		const Size payload_left = META_SIZE + size - offset;
		const Size max_payload_size = p_max_slice_size_with_meta - META_SIZE;
		const Size payload_size = MIN(payload_left, max_payload_size);

		// |<--offset->|<---size--->|
		// |           |            |
		// |   META    | Payload... |
		// | ID | PART | Payload... |
		const Size id_offset = offset - META_SIZE + 0;
		const Size part_offset = offset - META_SIZE + sizeof(id);
		encode_uint32(id,          storage.ptrw() + id_offset);
		encode_uint32(part_number, storage.ptrw() + part_offset);

		auto ret = Span<const uint8_t>(storage.ptr() + id_offset, META_SIZE + payload_size);

		offset += payload_size;
		++part_number;
		return ret;
	}

private:
	PackedByteArray::Size size{0}; // useful size, not including prepend
	PackedByteArray::Size offset{META_SIZE}; // to start of useful data, always has room for prepend behind
	PackedByteArray storage;
	ID id{0};
	Part part_number{0};
};

class BufferGluer {
public:

private:
};

struct SyncNodeHeader {
	bool is_parted;     // 1 bit
	uint32_t size;		// 11 bits, max 2047 bytes (fits into MTU)
	uint32_t part_num;		// 12 bits leftover, given 24 bits (3 bytes) for size, parts and is_parted
	uint32_t net_id;	// 32 bits
};

constexpr uint32_t IS_PARTED_BITS = 1;
constexpr uint32_t SIZE_BITS = 11;
constexpr uint32_t PARTS_BITS = 12;

constexpr uint32_t IS_PARTED_SHIFT = 0;
constexpr uint32_t SIZE_SHIFT = IS_PARTED_SHIFT + IS_PARTED_BITS;   // 1
constexpr uint32_t PARTS_SHIFT = SIZE_SHIFT + SIZE_BITS;			// 12

constexpr uint32_t IS_PARTED_MASK = (1u << IS_PARTED_BITS) - 1u;	// 0x001
constexpr uint32_t SIZE_MASK = (1u << SIZE_BITS) - 1u;				// 0x7FF
constexpr uint32_t PARTS_MASK = (1u << PARTS_BITS) - 1u;			// 0xFFF

static_assert(IS_PARTED_BITS + SIZE_BITS + PARTS_BITS == 24);

_FORCE_INLINE_ uint32_t pack_SyncNodeHeader(const SyncNodeHeader &p_header) {
	const uint32_t parts = p_header.is_parted ? p_header.parts : 0;
	return (uint32_t(p_header.is_parted ? 1 : 0) << IS_PARTED_SHIFT) |
			(uint32_t(p_header.size) << SIZE_SHIFT) |
			(uint32_t(parts) << PARTS_SHIFT);
}

_FORCE_INLINE_ unsigned int encode_SyncNodeHeader(const SyncNodeHeader &p_header, uint8_t *p_dst) {
	ERR_FAIL_COND_V_MSG(p_header.parts > PARTS_MASK, 0, "encode_SyncNodeHeader: parts does not fit in 12 bits.");
	ERR_FAIL_COND_V_MSG(p_header.size > SIZE_MASK, 0, "encode_SyncNodeHeader: size does not fit in 11 bits.");
	const uint32_t packed = pack_SyncNodeHeader(p_header);

	p_dst[0] = uint8_t((packed >> 0) & 0xFF);
	p_dst[1] = uint8_t((packed >> 8) & 0xFF);
	int offset = 2 * sizeof(uint8_t);

	if (p_header.is_parted) {
		p_dst[2] = uint8_t((packed >> 16) & 0xFF);
		offset += sizeof(uint8_t);
	}
	encode_uint32(p_header.net_id, p_dst + offset);
	offset += sizeof(uint32_t);

	return offset;
}

static _FORCE_INLINE_ SyncNodeHeader decode_SyncNodeHeader(const uint8_t *p_src) {
	const uint32_t packed =
			(uint32_t(p_src[0]) << 0) |
			(uint32_t(p_src[1]) << 8) |
			(uint32_t(p_src[2]) << 16);

	SyncNodeHeader header;
	header.is_parted = ((packed >> IS_PARTED_SHIFT) & IS_PARTED_MASK) != 0;
	header.size = uint32_t((packed >> SIZE_SHIFT) & SIZE_MASK);
	int offset = 2;

	header.parts = 1;
	if (header.is_parted) {
		header.parts = uint32_t((packed >> PARTS_SHIFT) & PARTS_MASK);
		offset += 1;
	}

	header.net_id = decode_uint32(p_src + offset);

	return header;
}
