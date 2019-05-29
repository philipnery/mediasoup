#define MS_CLASS "RTC::SvcConsumer"
// #define MS_LOG_DEV

#include "RTC/SvcConsumer.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Channel/Notifier.hpp"
#include "RTC/Codecs/Codecs.hpp"

namespace RTC
{
	/* Instance methods. */

	SvcConsumer::SvcConsumer(const std::string& id, RTC::Consumer::Listener* listener, json& data)
	  : RTC::Consumer::Consumer(id, listener, data, RTC::RtpParameters::Type::SVC)
	{
		MS_TRACE();

		// Ensure there is a single encoding.
		if (this->consumableRtpEncodings.size() != 1)
			MS_THROW_TYPE_ERROR("invalid consumableRtpEncodings with size != 1");

		auto& encoding = this->rtpParameters.encodings[0];

		// Ensure there are multiple spatial or temporal layers.
		if (encoding.spatialLayers < 2 && encoding.temporalLayers < 2)
			MS_THROW_TYPE_ERROR("invalid number of layers");

		auto jsonPreferredLayersIt = data.find("preferredLayers");

		// Set preferredLayers (if given).
		if (jsonPreferredLayersIt != data.end() && jsonPreferredLayersIt->is_object())
		{
			auto jsonSpatialLayerIt  = jsonPreferredLayersIt->find("spatialLayer");
			auto jsonTemporalLayerIt = jsonPreferredLayersIt->find("temporalLayer");

			if (jsonSpatialLayerIt == jsonPreferredLayersIt->end() || !jsonSpatialLayerIt->is_number_unsigned())
			{
				MS_THROW_TYPE_ERROR("missing preferredLayers.spatialLayer");
			}

			this->preferredSpatialLayer = jsonSpatialLayerIt->get<int16_t>();

			if (this->preferredSpatialLayer > encoding.spatialLayers - 1)
				this->preferredSpatialLayer = encoding.spatialLayers - 1;

			if (jsonTemporalLayerIt != jsonPreferredLayersIt->end() && jsonTemporalLayerIt->is_number_unsigned())
			{
				this->preferredTemporalLayer = jsonTemporalLayerIt->get<int16_t>();

				if (this->preferredTemporalLayer > encoding.temporalLayers - 1)
					this->preferredTemporalLayer = encoding.temporalLayers - 1;
			}
			else
			{
				this->preferredTemporalLayer = encoding.temporalLayers - 1;
			}
		}
		else
		{
			// Initially set preferredSpatialLayer and preferredTemporalLayer to the
			// maximum value.
			this->preferredSpatialLayer  = encoding.spatialLayers - 1;
			this->preferredTemporalLayer = encoding.temporalLayers - 1;
		}

		// Create the encoding context (if not available for this media codec, throw).
		auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		this->encodingContext.reset(RTC::Codecs::GetEncodingContext(mediaCodec->mimeType));

		if (!this->encodingContext)
			MS_THROW_TYPE_ERROR("media codec not supported with SVC");

		// Create RtpStreamSend instance for sending a single stream to the remote.
		CreateRtpStream();
	}

	SvcConsumer::~SvcConsumer()
	{
		MS_TRACE();

		delete this->rtpStream;
	}

	void SvcConsumer::FillJson(json& jsonObject) const
	{
		MS_TRACE();

		// Call the parent method.
		RTC::Consumer::FillJson(jsonObject);

		// Add rtpStream.
		this->rtpStream->FillJson(jsonObject["rtpStream"]);

		// Add preferredSpatialLayer.
		jsonObject["preferredSpatialLayer"] = this->preferredSpatialLayer;

		// Add targetSpatialLayer.
		jsonObject["targetSpatialLayer"] = this->encodingContext->GetTargetSpatialLayer();

		// Add currentSpatialLayer.
		jsonObject["currentSpatialLayer"] = this->encodingContext->GetCurrentSpatialLayer();

		// Add preferredTemporalLayer.
		jsonObject["preferredTemporalLayer"] = this->preferredTemporalLayer;

		// Add targetTemporalLayer.
		jsonObject["targetTemporalLayer"] = this->encodingContext->GetTargetTemporalLayer();

		// Add currentTemporalLayer.
		jsonObject["currentTemporalLayer"] = this->encodingContext->GetCurrentTemporalLayer();
	}

	void SvcConsumer::FillJsonStats(json& jsonArray) const
	{
		MS_TRACE();

		// Add stats of our send stream.
		jsonArray.emplace_back(json::value_t::object);
		this->rtpStream->FillJsonStats(jsonArray[0]);

		// Add stats of our recv stream.
		if (this->producerRtpStream)
		{
			jsonArray.emplace_back(json::value_t::object);
			this->producerRtpStream->FillJsonStats(jsonArray[1]);
		}
	}

	void SvcConsumer::FillJsonScore(json& jsonObject) const
	{
		MS_TRACE();

		jsonObject["score"] = this->rtpStream->GetScore();

		if (this->producerRtpStream)
			jsonObject["producerScore"] = this->producerRtpStream->GetScore();
		else
			jsonObject["producerScore"] = 0;
	}

	void SvcConsumer::HandleRequest(Channel::Request* request)
	{
		MS_TRACE();

		switch (request->methodId)
		{
			case Channel::Request::MethodId::CONSUMER_REQUEST_KEY_FRAME:
			{
				if (IsActive())
					RequestKeyFrame();

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::CONSUMER_SET_PREFERRED_LAYERS:
			{
				auto previousPreferredSpatialLayer  = this->preferredSpatialLayer;
				auto previousPreferredTemporalLayer = this->preferredTemporalLayer;

				auto jsonSpatialLayerIt  = request->data.find("spatialLayer");
				auto jsonTemporalLayerIt = request->data.find("temporalLayer");

				// Spatial layer.
				if (jsonSpatialLayerIt == request->data.end() || !jsonSpatialLayerIt->is_number_unsigned())
				{
					MS_THROW_TYPE_ERROR("missing spatialLayer");
				}

				this->preferredSpatialLayer = jsonSpatialLayerIt->get<int16_t>();

				if (this->preferredSpatialLayer > this->rtpStream->GetSpatialLayers() - 1)
					this->preferredSpatialLayer = this->rtpStream->GetSpatialLayers() - 1;

				// preferredTemporaLayer is optional.
				if (jsonTemporalLayerIt != request->data.end() && jsonTemporalLayerIt->is_number_unsigned())
				{
					this->preferredTemporalLayer = jsonTemporalLayerIt->get<int16_t>();

					if (this->preferredTemporalLayer > this->rtpStream->GetTemporalLayers() - 1)
						this->preferredTemporalLayer = this->rtpStream->GetTemporalLayers() - 1;
				}
				else
				{
					this->preferredTemporalLayer = this->rtpStream->GetTemporalLayers() - 1;
				}

				MS_DEBUG_DEV(
				  "preferred layers changed to [spatial:%" PRIi16 ", temporal:%" PRIi16 ", consumerId:%s]",
				  this->preferredSpatialLayer,
				  this->preferredTemporalLayer,
				  this->id.c_str());

				request->Accept();

				// clang-format off
				if (
					IsActive() &&
					(
						this->preferredSpatialLayer != previousPreferredSpatialLayer ||
						this->preferredTemporalLayer != previousPreferredTemporalLayer
					)
				)
				// clang-format on
				{
					MayChangeLayers(/*force*/ true);
				}

				break;
			}

			default:
			{
				// Pass it to the parent class.
				RTC::Consumer::HandleRequest(request);
			}
		}
	}

	void SvcConsumer::ProducerRtpStream(RTC::RtpStream* rtpStream, uint32_t /*mappedSsrc*/)
	{
		MS_TRACE();

		this->producerRtpStream = rtpStream;

		// Emit the score event.
		EmitScore();
	}

	void SvcConsumer::ProducerNewRtpStream(RTC::RtpStream* rtpStream, uint32_t /*mappedSsrc*/)
	{
		MS_TRACE();

		this->producerRtpStream = rtpStream;

		// Emit the score event.
		EmitScore();

		if (IsActive())
			MayChangeLayers();
	}

	void SvcConsumer::ProducerRtpStreamScore(
	  RTC::RtpStream* /*rtpStream*/, uint8_t score, uint8_t previousScore)
	{
		MS_TRACE();

		// Emit score event.
		EmitScore();

		if (RTC::Consumer::IsActive())
		{
			// Just check target layers if the stream has died or reborned.
			//
			// clang-format off
			if (
				!this->externallyManagedBitrate ||
				(score == 0 || previousScore == 0)
			)
			// clang-format on
			{
				MayChangeLayers();
			}
		}
	}

	void SvcConsumer::ProducerRtcpSenderReport(RTC::RtpStream* rtpStream, bool first)
	{
		MS_TRACE();

		// Just interested if this is the first Sender Report for a RTP stream.
		if (first)
			MS_DEBUG_TAG(simulcast, "first SenderReport [ssrc:%" PRIu32 "]", rtpStream->GetSsrc());
		else
			return;

		// If our RTP stream does not yet have SR, do nothing since
		// we know we won't be able to switch.
		if (!this->producerRtpStream || !this->producerRtpStream->GetSenderReportNtpMs())
			return;

		if (IsActive())
			MayChangeLayers();
	}

	void SvcConsumer::SetExternallyManagedBitrate()
	{
		MS_TRACE();

		this->externallyManagedBitrate = true;
	}

	int16_t SvcConsumer::GetBitratePriority() const
	{
		MS_TRACE();

		if (!RTC::Consumer::IsActive())
			return 0;

		// Return a 0 priority if score of Producer stream is 0.
		if (!this->producerRtpStream || this->producerRtpStream->GetScore() == 0)
			return 0;

		int16_t prioritySpatialLayer{ 0 };

		// Otherwise, take the maximum spatial layer up to the preferred one.
		for (size_t idx{ 0 }; idx < this->producerRtpStream->GetSpatialLayers(); ++idx)
		{
			auto spatialLayer = static_cast<int16_t>(idx);

			// Do not choose a layer greater than the preferred one if we already found
			// an available layer equal or less than the preferred one.
			// TODO: Does this 'prioritySpatialLayer' check make any sense?
			if (spatialLayer > this->preferredSpatialLayer && prioritySpatialLayer >= -1)
				break;

			// Choose this layer for now.
			prioritySpatialLayer = spatialLayer;
		}

		// Return the choosen spatial layer plus one.
		return prioritySpatialLayer + 1;
	}

	uint32_t SvcConsumer::UseAvailableBitrate(uint32_t bitrate)
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		if (!RTC::Consumer::IsActive())
			return 0;

		// Calculate virtual available bitrate based on given bitrate and our
		// packet lost fraction.
		uint32_t virtualBitrate;
		auto lossPercentage = this->rtpStream->GetLossPercentage();

		// TODO: We may have to not consider fraction lost with Transport-CC.
		if (lossPercentage < 2)
			virtualBitrate = 1.08 * bitrate;
		else if (lossPercentage > 10)
			virtualBitrate = (1 - 0.5 * (lossPercentage / 100)) * bitrate;
		else
			virtualBitrate = bitrate;

		this->provisionalTargetSpatialLayer  = -1;
		this->provisionalTargetTemporalLayer = -1;

		uint32_t usedBitrate{ 0 };
		auto now = DepLibUV::GetTime();

		if (!this->producerRtpStream)
			goto done;

		if (this->producerRtpStream->GetScore() < 7)
			goto done;

		for (size_t idx{ 0 }; idx < this->producerRtpStream->GetSpatialLayers(); ++idx)
		{
			auto spatialLayer = static_cast<int16_t>(idx);
			int16_t temporalLayer{ 0 };

			// Check bitrate of every layer.
			for (; temporalLayer < this->producerRtpStream->GetTemporalLayers(); ++temporalLayer)
			{
				auto requiredBitrate = this->producerRtpStream->GetBitrate(now, spatialLayer, temporalLayer);

				MS_DEBUG_DEV(
				  "testing layers %" PRIi16 ":%" PRIi16 " [virtualBitrate:%" PRIu32
				  ", requiredBitrate:%" PRIu32 "]",
				  spatialLayer,
				  temporalLayer,
				  virtualBitrate,
				  requiredBitrate);

				// If layer is not being received, continue.
				if (requiredBitrate == 0)
					goto done;

				// If this layer requires more bitrate than the given one, abort the loop
				// (so use the previous chosen layers if any).
				if (requiredBitrate > virtualBitrate)
					goto done;

				// Set provisional layers and used bitrate.
				this->provisionalTargetSpatialLayer  = spatialLayer;
				this->provisionalTargetTemporalLayer = temporalLayer;
				usedBitrate                          = requiredBitrate;

				// If this is the preferred spatial and temporal layer, exit the loops.
				// clang-format off
				if (
					this->provisionalTargetSpatialLayer == this->preferredSpatialLayer &&
					this->provisionalTargetTemporalLayer == this->preferredTemporalLayer
				)
				// clang-format on
				{
					goto done;
				}
			}

			// If this is the preferred or higher spatial layer and has good score,
			// take it and exit.
			if (spatialLayer >= this->preferredSpatialLayer)
				break;
		}

	done:

		MS_DEBUG_2TAGS(
		  bwe,
		  simulcast,
		  "choosing layers %" PRIi16 ":%" PRIi16 " [bitrate:%" PRIu32 ", virtualBitrate:%" PRIu32
		  ", usedBitrate:%" PRIu32 ", consumerId:%s]",
		  this->provisionalTargetSpatialLayer,
		  this->provisionalTargetTemporalLayer,
		  bitrate,
		  virtualBitrate,
		  usedBitrate,
		  this->id.c_str());

		// Must recompute usedBitrate based on given bitrate, virtualBitrate and
		// usedBitrate.
		if (usedBitrate <= bitrate)
			return usedBitrate;
		else if (usedBitrate <= virtualBitrate)
			return bitrate;
		else
			return usedBitrate;
	}

	uint32_t SvcConsumer::IncreaseLayer(uint32_t bitrate)
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		if (!RTC::Consumer::IsActive())
			return 0;

		// If already in the preferred layers, do nothing.
		// clang-format off
		if (
			this->provisionalTargetSpatialLayer == this->preferredSpatialLayer &&
			this->provisionalTargetTemporalLayer == this->preferredTemporalLayer
		)
		// clang-format on
		{
			return 0;
		}

		// Calculate virtual available bitrate based on given bitrate and our
		// packet lost fraction.
		uint32_t virtualBitrate;
		auto lossPercentage = this->rtpStream->GetLossPercentage();

		// TODO: We may have to not consider fraction lost with Transport-CC.
		if (lossPercentage < 2)
			virtualBitrate = 1.08 * bitrate;
		else if (lossPercentage > 10)
			virtualBitrate = (1 - 0.5 * (lossPercentage / 100)) * bitrate;
		else
			virtualBitrate = bitrate;

		auto spatialLayer  = this->provisionalTargetSpatialLayer;
		auto temporalLayer = this->provisionalTargetTemporalLayer;

		// May upgrade from no spatial layer to spatial layer 0.
		if (spatialLayer == -1)
		{
			// Take it even if it's bad.
			if (this->producerRtpStream && this->producerRtpStream->GetScore() > 0)
			{
				spatialLayer  = 0;
				temporalLayer = 0;
			}
			else
			{
				// Must return now since we do not even have a producerRtpStream.
				return 0;
			}
		}
		// May upgrade temporal layer.
		else if (temporalLayer < this->producerRtpStream->GetTemporalLayers() - 1)
		{
			++temporalLayer;
		}
		// May upgrade spatial layer.
		else
		{
			// Producer stream does not exist or it's not good. Exit.
			if (!this->producerRtpStream || this->producerRtpStream->GetScore() < 7)
				return 0;

			// Set temporal layer to 0.
			temporalLayer = 0;
		}

		auto now             = DepLibUV::GetTime();
		auto requiredBitrate = this->producerRtpStream->GetLayerBitrate(now, 0, temporalLayer);

		// No luck.
		if (requiredBitrate > virtualBitrate)
			return 0;

		// Set provisional layers.
		this->provisionalTargetSpatialLayer  = spatialLayer;
		this->provisionalTargetTemporalLayer = temporalLayer;

		MS_DEBUG_DEV(
		  "upgrading to layers %" PRIi16 ":%" PRIi16 " [virtualBitrate:%" PRIu32
		  ", requiredBitrate:%" PRIu32 "]",
		  this->provisionalTargetSpatialLayer,
		  this->provisionalTargetTemporalLayer,
		  virtualBitrate,
		  requiredBitrate);

		if (requiredBitrate <= bitrate)
			return requiredBitrate;
		else if (requiredBitrate <= virtualBitrate)
			return bitrate;
		else
			return requiredBitrate; // NOTE: This cannot happen.
	}

	void SvcConsumer::ApplyLayers()
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		auto provisionalTargetSpatialLayer  = this->provisionalTargetSpatialLayer;
		auto provisionalTargetTemporalLayer = this->provisionalTargetTemporalLayer;

		// Reset provisional target layers.
		this->provisionalTargetSpatialLayer  = -1;
		this->provisionalTargetTemporalLayer = -1;

		if (!RTC::Consumer::IsActive())
			return;

		// clang-format off
		if (
			provisionalTargetSpatialLayer != this->encodingContext->GetTargetSpatialLayer() ||
			provisionalTargetTemporalLayer != this->encodingContext->GetTargetTemporalLayer()
		)
		// clang-format on
		{
			UpdateTargetLayers(provisionalTargetSpatialLayer, provisionalTargetTemporalLayer);
		}
	}

	void SvcConsumer::SendRtpPacket(RTC::RtpPacket* packet)
	{
		MS_TRACE();

		if (!IsActive())
			return;

		auto payloadType = packet->GetPayloadType();

		// NOTE: This may happen if this Consumer supports just some codecs of those
		// in the corresponding Producer.
		if (this->supportedCodecPayloadTypes.find(payloadType) == this->supportedCodecPayloadTypes.end())
		{
			MS_DEBUG_DEV("payload type not supported [payloadType:%" PRIu8 "]", payloadType);

			return;
		}

		// If we need to sync and this is not a key frame, ignore the packet.
		if (this->syncRequired && !packet->IsKeyFrame())
			return;

		// Whether this is the first packet after re-sync.
		bool isSyncPacket = this->syncRequired;

		// Sync sequence number and timestamp if required.
		if (isSyncPacket)
		{
			if (packet->IsKeyFrame())
				MS_DEBUG_TAG(rtp, "sync key frame received");

			this->rtpSeqManager.Sync(packet->GetSequenceNumber() - 1);

			this->syncRequired = false;
		}

		// TMP
		// MS_ERROR(
		// "spatialLayer:%d, this->encodingContext->GetTargetSpatialLayer():%d,
		// this->encodingContext->GetTargetTemporalLayer():%d", packet->GetSpatialLayer(),
		// this->encodingContext->GetTargetSpatialLayer(),
		// this->encodingContext->GetTargetTemporalLayer());

		auto previousSpatialLayer  = this->encodingContext->GetCurrentSpatialLayer();
		auto previousTemporalLayer = this->encodingContext->GetCurrentTemporalLayer();

		if (!packet->ProcessPayload(this->encodingContext.get()))
		{
			this->rtpSeqManager.Drop(packet->GetSequenceNumber());

			return;
		}

		// clang-format off
		if (
			previousSpatialLayer != this->encodingContext->GetCurrentSpatialLayer() ||
			previousTemporalLayer != this->encodingContext->GetCurrentTemporalLayer()
		)
		// clang-format on
		{
			// Emit the layersChange event.
			EmitLayersChange();
		}

		// Update RTP seq number and timestamp based on NTP offset.
		uint16_t seq;

		this->rtpSeqManager.Input(packet->GetSequenceNumber(), seq);

		// Save original packet fields.
		auto origSsrc = packet->GetSsrc();
		auto origSeq  = packet->GetSequenceNumber();

		// Rewrite packet.
		packet->SetSsrc(this->rtpParameters.encodings[0].ssrc);
		packet->SetSequenceNumber(seq);

		if (isSyncPacket)
		{
			MS_DEBUG_TAG(
			  rtp,
			  "sending sync packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSeq);
		}

		// Process the packet.
		if (this->rtpStream->ReceivePacket(packet))
		{
			// Send the packet.
			this->listener->OnConsumerSendRtpPacket(this, packet);
		}
		else
		{
			MS_WARN_TAG(
			  rtp,
			  "failed to send packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [ssrc:%" PRIu32 ", seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSsrc,
			  origSeq);
		}

		// Restore packet fields.
		packet->SetSsrc(origSsrc);
		packet->SetSequenceNumber(origSeq);

		// Restore the original payload if needed.
		packet->RestorePayload();
	}

	void SvcConsumer::SendProbationRtpPacket(uint16_t seq)
	{
		MS_TRACE();

		this->rtpStream->SendProbationRtpPacket(seq);
	}

	void SvcConsumer::GetRtcp(RTC::RTCP::CompoundPacket* packet, RTC::RtpStreamSend* rtpStream, uint64_t now)
	{
		MS_TRACE();

		MS_ASSERT(rtpStream == this->rtpStream, "RTP stream does not match");

		if (static_cast<float>((now - this->lastRtcpSentTime) * 1.15) < this->maxRtcpInterval)
			return;

		auto* report = this->rtpStream->GetRtcpSenderReport(now);

		if (!report)
			return;

		packet->AddSenderReport(report);

		// Build SDES chunk for this sender.
		auto* sdesChunk = this->rtpStream->GetRtcpSdesChunk();

		packet->AddSdesChunk(sdesChunk);

		this->lastRtcpSentTime = now;
	}

	void SvcConsumer::NeedWorstRemoteFractionLost(uint32_t /*mappedSsrc*/, uint8_t& worstRemoteFractionLost)
	{
		MS_TRACE();

		if (!IsActive())
			return;

		auto fractionLost = this->rtpStream->GetFractionLost();

		// If our fraction lost is worse than the given one, update it.
		if (fractionLost > worstRemoteFractionLost)
			worstRemoteFractionLost = fractionLost;
	}

	void SvcConsumer::ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket)
	{
		MS_TRACE();

		if (!IsActive())
			return;

		this->rtpStream->ReceiveNack(nackPacket);
	}

	void SvcConsumer::ReceiveKeyFrameRequest(
	  RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t /*ssrc*/)
	{
		MS_TRACE();

		this->rtpStream->ReceiveKeyFrameRequest(messageType);

		if (IsActive())
			RequestKeyFrame();
	}

	void SvcConsumer::ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();

		this->rtpStream->ReceiveRtcpReceiverReport(report);
	}

	uint32_t SvcConsumer::GetTransmissionRate(uint64_t now)
	{
		MS_TRACE();

		if (!IsActive())
			return 0u;

		return this->rtpStream->GetBitrate(now);
	}

	void SvcConsumer::UserOnTransportConnected()
	{
		MS_TRACE();

		this->syncRequired = true;

		if (IsActive())
			MayChangeLayers();
	}

	void SvcConsumer::UserOnTransportDisconnected()
	{
		MS_TRACE();

		this->rtpStream->Pause();

		UpdateTargetLayers(-1, -1);
	}

	void SvcConsumer::UserOnPaused()
	{
		MS_TRACE();

		this->rtpStream->Pause();

		UpdateTargetLayers(-1, -1);

		// Tell the transport so it can distribute available bitrate into other
		// consumers.
		if (this->externallyManagedBitrate)
			this->listener->OnConsumerNeedBitrateChange(this);
	}

	void SvcConsumer::UserOnResumed()
	{
		MS_TRACE();

		this->syncRequired = true;

		if (IsActive())
			MayChangeLayers();
	}

	void SvcConsumer::CreateRtpStream()
	{
		MS_TRACE();

		auto& encoding   = this->rtpParameters.encodings[0];
		auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		MS_DEBUG_TAG(
		  rtp, "[ssrc:%" PRIu32 ", payloadType:%" PRIu8 "]", encoding.ssrc, mediaCodec->payloadType);

		// Set stream params.
		RTC::RtpStream::Params params;

		params.ssrc           = encoding.ssrc;
		params.payloadType    = mediaCodec->payloadType;
		params.mimeType       = mediaCodec->mimeType;
		params.clockRate      = mediaCodec->clockRate;
		params.cname          = this->rtpParameters.rtcp.cname;
		params.spatialLayers  = encoding.spatialLayers;
		params.temporalLayers = encoding.temporalLayers;

		// Check in band FEC in codec parameters.
		if (mediaCodec->parameters.HasInteger("useinbandfec") && mediaCodec->parameters.GetInteger("useinbandfec") == 1)
		{
			MS_DEBUG_TAG(rtp, "in band FEC enabled");

			params.useInBandFec = true;
		}

		// Check DTX in codec parameters.
		if (mediaCodec->parameters.HasInteger("usedtx") && mediaCodec->parameters.GetInteger("usedtx") == 1)
		{
			MS_DEBUG_TAG(rtp, "DTX enabled");

			params.useDtx = true;
		}

		// Check DTX in the encoding.
		if (encoding.dtx)
		{
			MS_DEBUG_TAG(rtp, "DTX enabled");

			params.useDtx = true;
		}

		for (auto& fb : mediaCodec->rtcpFeedback)
		{
			if (!params.useNack && fb.type == "nack" && fb.parameter == "")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "NACK supported");

				params.useNack = true;
			}
			else if (!params.usePli && fb.type == "nack" && fb.parameter == "pli")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "PLI supported");

				params.usePli = true;
			}
			else if (!params.useFir && fb.type == "ccm" && fb.parameter == "fir")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "FIR supported");

				params.useFir = true;
			}
		}

		// Create a RtpStreamSend for sending a single media stream.
		size_t bufferSize = params.useNack ? 600 : 0;

		this->rtpStream = new RTC::RtpStreamSend(this, params, bufferSize);
		this->rtpStreams.push_back(this->rtpStream);

		// If the Consumer is paused, tell the RtpStreamSend.
		if (IsPaused() || IsProducerPaused())
			this->rtpStream->Pause();

		auto* rtxCodec = this->rtpParameters.GetRtxCodecForEncoding(encoding);

		if (rtxCodec && encoding.hasRtx)
			this->rtpStream->SetRtx(rtxCodec->payloadType, encoding.rtx.ssrc);
	}

	void SvcConsumer::RequestKeyFrame()
	{
		MS_TRACE();

		if (this->kind != RTC::Media::Kind::VIDEO)
			return;

		auto mappedSsrc = this->consumableRtpEncodings[0].ssrc;

		this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc);
	}

	void SvcConsumer::MayChangeLayers(bool force)
	{
		MS_TRACE();

		int16_t newTargetSpatialLayer;
		int16_t newTargetTemporalLayer;

		if (RecalculateTargetLayers(newTargetSpatialLayer, newTargetTemporalLayer))
		{
			// If bitrate externally managed, don't bother the transport unless
			// the newTargetSpatialLayer has changed (or force is true).
			// This is because, if bitrate is externally managed, the target temporal
			// layer is managed by the available given bitrate so the transport
			// will let us change it when it considers.
			if (this->externallyManagedBitrate)
			{
				if (newTargetSpatialLayer != this->encodingContext->GetTargetSpatialLayer() || force)
					this->listener->OnConsumerNeedBitrateChange(this);
			}
			else
			{
				UpdateTargetLayers(newTargetSpatialLayer, newTargetTemporalLayer);
			}
		}
	}

	bool SvcConsumer::RecalculateTargetLayers(
	  int16_t& newTargetSpatialLayer, int16_t& newTargetTemporalLayer) const
	{
		MS_TRACE();

		// Start with no layers.
		newTargetSpatialLayer  = -1;
		newTargetTemporalLayer = -1;

		auto now = DepLibUV::GetTime();

		if (!this->producerRtpStream)
			goto done;

		if (this->producerRtpStream->GetScore() == 0)
			goto done;

		for (size_t idx{ 0 }; idx < this->producerRtpStream->GetSpatialLayers(); ++idx)
		{
			auto spatialLayer = static_cast<int16_t>(idx);

			if (producerRtpStream->GetBitrate(now, spatialLayer, 0))
			{
				newTargetSpatialLayer = spatialLayer;

				// If this is the preferred or higher spatial layer and has bitrate,
				// take it and exit.
				if (spatialLayer >= this->preferredSpatialLayer)
					break;
			}
		}

		if (newTargetSpatialLayer != -1)
		{
			if (newTargetSpatialLayer == this->preferredSpatialLayer)
				newTargetTemporalLayer = this->preferredTemporalLayer;
			else if (newTargetSpatialLayer < this->preferredSpatialLayer)
				newTargetTemporalLayer = this->rtpStream->GetTemporalLayers() - 1;
			else
				newTargetTemporalLayer = 0;
		}

	done:

		// Return true if any target layer changed.
		// clang-format off
		return (
			newTargetSpatialLayer != this->encodingContext->GetTargetSpatialLayer() ||
			newTargetTemporalLayer != this->encodingContext->GetTargetTemporalLayer()
		);
		// clang-format on
	}

	void SvcConsumer::UpdateTargetLayers(int16_t newTargetSpatialLayer, int16_t newTargetTemporalLayer)
	{
		MS_TRACE();

		if (newTargetSpatialLayer == -1)
		{
			// Unset current and target layers.
			this->encodingContext->SetTargetSpatialLayer(-1);
			this->encodingContext->SetCurrentSpatialLayer(-1);
			this->encodingContext->SetTargetTemporalLayer(-1);
			this->encodingContext->SetCurrentTemporalLayer(-1);

			MS_DEBUG_TAG(
			  simulcast, "target layers changed [spatial:-1, temporal:-1, consumerId:%s]", this->id.c_str());

			EmitLayersChange();

			return;
		}

		this->encodingContext->SetTargetSpatialLayer(newTargetSpatialLayer);
		this->encodingContext->SetTargetTemporalLayer(newTargetTemporalLayer);

		MS_DEBUG_TAG(
		  simulcast,
		  "target layers changed [spatial:%" PRIi16 ", temporal:%" PRIi16 ", consumerId:%s]",
		  newTargetSpatialLayer,
		  newTargetTemporalLayer,
		  this->id.c_str());

		// If the target spatial layer is higher than the current one, request
		// a key frame.
		if (this->encodingContext->GetTargetSpatialLayer() > this->encodingContext->GetCurrentSpatialLayer())
		{
			RequestKeyFrame();
		}
	}

	inline void SvcConsumer::EmitScore() const
	{
		MS_TRACE();

		json data = json::object();

		FillJsonScore(data);

		Channel::Notifier::Emit(this->id, "score", data);
	}

	inline void SvcConsumer::EmitLayersChange() const
	{
		MS_TRACE();

		MS_DEBUG_DEV(
		  "current layers changed to [spatial:%" PRIi16 ", temporal:%" PRIi16 ", consumerId:%s]",
		  this->encodingContext->GetCurrentSpatialLayer(),
		  this->encodingContext->GetCurrentTemporalLayer(),
		  this->id.c_str());

		json data = json::object();

		if (this->encodingContext->GetCurrentSpatialLayer() >= 0)
		{
			data["spatialLayer"]  = this->encodingContext->GetCurrentSpatialLayer();
			data["temporalLayer"] = this->encodingContext->GetCurrentTemporalLayer();
		}
		else
		{
			data = nullptr;
		}

		Channel::Notifier::Emit(this->id, "layerschange", data);
	}

	inline void SvcConsumer::OnRtpStreamScore(
	  RTC::RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/)
	{
		MS_TRACE();

		// Emit the score event.
		EmitScore();

		if (IsActive())
		{
			// Just check target layers if our bitrate is not externally managed.
			// NOTE: For now this is a bit useless since, when locally managed, we do
			// not check the Consumer score at all.
			if (!this->externallyManagedBitrate)
				MayChangeLayers();
		}
	}

	inline void SvcConsumer::OnRtpStreamRetransmitRtpPacket(
	  RTC::RtpStreamSend* /*rtpStream*/, RTC::RtpPacket* packet, bool probation)
	{
		MS_TRACE();

		this->listener->OnConsumerRetransmitRtpPacket(this, packet, probation);
	}
} // namespace RTC
