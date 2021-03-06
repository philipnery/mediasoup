#define MS_CLASS "RTC::RTCP::FeedbackPsFir"
// #define MS_LOG_DEV

#include "RTC/RTCP/FeedbackPsFir.hpp"
#include "Logger.hpp"
#include <cstring>
#include <cstring> // std::memset()

namespace RTC
{
	namespace RTCP
	{
		/* Class methods. */

		/* Instance methods. */

		FeedbackPsFirItem::FeedbackPsFirItem(uint32_t ssrc, uint8_t sequenceNumber)
		{
			MS_TRACE();

			this->raw    = new uint8_t[sizeof(Header)];
			this->header = reinterpret_cast<Header*>(this->raw);

			// Set reserved bits to zero.
			std::memset(this->header, 0, sizeof(Header));

			this->header->ssrc           = uint32_t{ htonl(ssrc) };
			this->header->sequenceNumber = sequenceNumber;
		}

		size_t FeedbackPsFirItem::Serialize(uint8_t* buffer)
		{
			MS_TRACE();

			std::memcpy(buffer, this->header, sizeof(Header));

			return sizeof(Header);
		}

		void FeedbackPsFirItem::Dump() const
		{
			MS_TRACE();

			MS_DEBUG_DEV("<FeedbackPsFirItem>");
			MS_DEBUG_DEV("  ssrc            : %" PRIu32, this->GetSsrc());
			MS_DEBUG_DEV("  sequence number : %" PRIu8, this->GetSequenceNumber());
			MS_DEBUG_DEV("</FeedbackPsFirItem>");
		}
	} // namespace RTCP
} // namespace RTC
