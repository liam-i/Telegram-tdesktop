/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/concurrent_sender.h"

#include "mtproto/mtp_instance.h"
#include "mtproto/rpc_sender.h"
#include "mtproto/session.h"

namespace MTP {

class ConcurrentSender::RPCDoneHandler : public RPCAbstractDoneHandler {
public:
	RPCDoneHandler(
		not_null<ConcurrentSender*> sender,
		Fn<void(FnMut<void()>)> runner);

	void operator()(
		mtpRequestId requestId,
		const mtpPrime *from,
		const mtpPrime *end) override;

private:
	base::weak_ptr<ConcurrentSender> _weak;
	Fn<void(FnMut<void()>)> _runner;

};

class ConcurrentSender::RPCFailHandler : public RPCAbstractFailHandler {
public:
	RPCFailHandler(
		not_null<ConcurrentSender*> sender,
		Fn<void(FnMut<void()>)> runner,
		FailSkipPolicy skipPolicy);

	bool operator()(
		mtpRequestId requestId,
		const RPCError &error) override;

private:
	base::weak_ptr<ConcurrentSender> _weak;
	Fn<void(FnMut<void()>)> _runner;
	FailSkipPolicy _skipPolicy = FailSkipPolicy::Simple;

};

ConcurrentSender::RPCDoneHandler::RPCDoneHandler(
	not_null<ConcurrentSender*> sender,
	Fn<void(FnMut<void()>)> runner)
: _weak(sender)
, _runner(std::move(runner)) {
}

void ConcurrentSender::RPCDoneHandler::operator()(
		mtpRequestId requestId,
		const mtpPrime *from,
		const mtpPrime *end) {
	auto response = gsl::make_span(
		from,
		end - from);
	_runner([=, weak = _weak, moved = bytes::make_vector(response)]() mutable {
		if (const auto strong = weak.get()) {
			strong->senderRequestDone(requestId, std::move(moved));
		}
	});
}

ConcurrentSender::RPCFailHandler::RPCFailHandler(
	not_null<ConcurrentSender*> sender,
	Fn<void(FnMut<void()>)> runner,
	FailSkipPolicy skipPolicy)
: _weak(sender)
, _runner(std::move(runner))
, _skipPolicy(skipPolicy) {
}

bool ConcurrentSender::RPCFailHandler::operator()(
		mtpRequestId requestId,
		const RPCError &error) {
	if (_skipPolicy == FailSkipPolicy::Simple) {
		if (MTP::isDefaultHandledError(error)) {
			return false;
		}
	} else if (_skipPolicy == FailSkipPolicy::HandleFlood) {
		if (MTP::isDefaultHandledError(error) && !MTP::isFloodError(error)) {
			return false;
		}
	}
	_runner([=, weak = _weak, error = error]() mutable {
		if (const auto strong = weak.get()) {
			strong->senderRequestFail(requestId, std::move(error));
		}
	});
	return true;
}

template <typename Method>
auto ConcurrentSender::with_instance(Method &&method)
-> std::enable_if_t<is_callable_v<Method, not_null<Instance*>>> {
	crl::on_main([method = std::forward<Method>(method)]() mutable {
		if (const auto instance = MainInstance()) {
			std::move(method)(instance);
		}
	});
}

ConcurrentSender::RequestBuilder::RequestBuilder(
	not_null<ConcurrentSender*> sender,
	mtpRequest &&serialized) noexcept
: _sender(sender)
, _serialized(std::move(serialized)) {
}

void ConcurrentSender::RequestBuilder::setToDC(ShiftedDcId dcId) noexcept {
	_dcId = dcId;
}

void ConcurrentSender::RequestBuilder::setCanWait(TimeMs ms) noexcept {
	_canWait = ms;
}

void ConcurrentSender::RequestBuilder::setFailSkipPolicy(
		FailSkipPolicy policy) noexcept {
	_failSkipPolicy = policy;
}

void ConcurrentSender::RequestBuilder::setAfter(
		mtpRequestId requestId) noexcept {
	_afterRequestId = requestId;
}

mtpRequestId ConcurrentSender::RequestBuilder::send() {
	const auto requestId = GetNextRequestId();
	const auto dcId = _dcId;
	const auto msCanWait = _canWait;
	const auto afterRequestId = _afterRequestId;

	_sender->senderRequestRegister(requestId, std::move(_handlers));
	_sender->with_instance([
		=,
		request = std::move(_serialized),
		done = std::make_shared<RPCDoneHandler>(_sender, _sender->_runner),
		fail = std::make_shared<RPCFailHandler>(
			_sender,
			_sender->_runner,
			_failSkipPolicy)
	](not_null<Instance*> instance) mutable {
		instance->sendSerialized(
			requestId,
			std::move(request),
			RPCResponseHandler(std::move(done), std::move(fail)),
			dcId,
			msCanWait,
			afterRequestId);
	});

	return requestId;
}

ConcurrentSender::ConcurrentSender(Fn<void(FnMut<void()>)> runner)
: _runner(runner) {
}

ConcurrentSender::~ConcurrentSender() {
	senderRequestCancelAll();
}

void ConcurrentSender::senderRequestRegister(
		mtpRequestId requestId,
		Handlers &&handlers) {
	_requests.emplace(requestId, std::move(handlers));
}

void ConcurrentSender::senderRequestDone(
		mtpRequestId requestId,
		bytes::const_span result) {
	if (auto handlers = _requests.take(requestId)) {
		try {
			std::move(handlers->done)(requestId, result);
		} catch (Exception &e) {
			std::move(handlers->fail)(requestId, internal::rpcClientError(
				"RESPONSE_PARSE_FAILED",
				QString("exception text: ") + e.what()));
		}
	}
}

void ConcurrentSender::senderRequestFail(
		mtpRequestId requestId,
		RPCError &&error) {
	if (auto handlers = _requests.take(requestId)) {
		std::move(handlers->fail)(requestId, std::move(error));
	}
}

void ConcurrentSender::senderRequestCancel(mtpRequestId requestId) {
	senderRequestDetach(requestId);
	with_instance([=](not_null<Instance*> instance) {
		instance->cancel(requestId);
	});
}

void ConcurrentSender::senderRequestCancelAll() {
	auto list = std::vector<mtpRequestId>(_requests.size());
	for (const auto &[requestId, handlers] : base::take(_requests)) {
		list.push_back(requestId);
	}
	with_instance([list = std::move(list)](not_null<Instance*> instance) {
		for (const auto requestId : list) {
			instance->cancel(requestId);
		}
	});
}

void ConcurrentSender::senderRequestDetach(mtpRequestId requestId) {
	_requests.erase(requestId);
}

} // namespace MTP
