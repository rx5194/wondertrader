#include "WtHftStraOrderBook.h"
#include "../Includes/IHftStraCtx.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"

extern const char* FACT_NAME;

WtHftStraOrderBook::WtHftStraOrderBook(const char* id)
	: HftStrategy(id)
	, _last_tick(NULL)
	, _last_entry_time(UINT64_MAX)
	, _channel_ready(false)
	, _last_calc_time(92500)
	, _unit(1)
	, _cancel_cnt(0)
	, observationTime(30)
	, imbalanceLevel(4)
	, imbalanceRatio(3.0)
	, penaltyFactor(2.0)
	, marketDir(0)
	, strategyDir(0)
{
}


WtHftStraOrderBook::~WtHftStraOrderBook()
{
	if (_last_tick)
		_last_tick->release();
}

const char* WtHftStraOrderBook::getName()
{
	return "WtHftStraOrderBook";
}

const char* WtHftStraOrderBook::getFactName()
{
	return FACT_NAME;
}

bool WtHftStraOrderBook::init(WTSVariant* cfg)
{
	_code = cfg->getCString("code");
	_secs = cfg->getUInt32("second");
	_offset = cfg->getUInt32("offset");

	observationTime = cfg->getUInt32("observationTime");
	imbalanceLevel = cfg->getUInt32("imbalanceLevel");
	imbalanceRatio = cfg->getDouble("imbalanceRatio");
	penaltyFactor = cfg->getDouble("penaltyFactor");
	return true;
}

void WtHftStraOrderBook::on_entrust(uint32_t localid, bool bSuccess, const char* message, const char* userTag)
{

}

void WtHftStraOrderBook::on_init(IHftStraCtx* ctx)
{
	WTSTickSlice* ticks = ctx->stra_get_ticks(_code.c_str(), 1);
	if (ticks)
		ticks->release();

	/*WTSKlineSlice* kline = ctx->stra_get_bars(_code.c_str(), "m1", 30);
	if (kline)
		kline->release();*/

	ctx->stra_sub_ticks(_code.c_str());

	_ctx = ctx;
}

void WtHftStraOrderBook::on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick)
{	
	if (_code.compare(code) != 0)
		return;

	if (!_orders.empty())
	{
		check_orders();
		return;
	}

	if (!_channel_ready)
		return;

	WTSTickData* curTick = ctx->stra_get_last_tick(code);
	if (curTick)
		curTick->release();

	updateDistribution(newTick->getTickStruct());
	benchLasttick = newTick->getTickStruct();
	marketDir = benchInstDir();


	uint32_t curMin = newTick->actiontime() / 1000;	//actiontime是带毫秒的,要取得s,则需要除以1000
	if (curMin - _last_calc_time > observationTime)
	{
		if (strategyDir * marketDir >= 0)
		{
			strategyDir += marketDir;
		}
		else if (marketDir > 0)
		{
			strategyDir += penaltyFactor;
		}
		else
		{
			strategyDir -= penaltyFactor;
		}

		_last_calc_time = curMin;
		for (auto iter = distributionOverTime.begin(); iter != distributionOverTime.end(); iter = distributionOverTime.erase(iter)) {}
	}

	uint64_t now = TimeUtils::makeTime(ctx->stra_get_date(), ctx->stra_get_time() * 100000 + ctx->stra_get_secs());
	if (strategyDir != 0)
	{
		double curPos = ctx->stra_get_position(code);
		double price = newTick->price();
		WTSCommodityInfo* cInfo = ctx->stra_get_comminfo(code);

		if(strategyDir > 0  && curPos <= 0)
		{//正向信号,且当前仓位小于等于0
			//最新价+2跳下单
			double targetPx = price + cInfo->getPriceTick() * _offset;
			auto ids = ctx->stra_buy(code, targetPx, _unit, "enterlong");

			_mtx_ords.lock();
			for( auto localid : ids)
			{
				_orders.insert(localid);
			}
			_mtx_ords.unlock();
			_last_entry_time = now;
		}
		else if (strategyDir < 0 )
		{
			double targetPx = price - cInfo->getPriceTick()*_offset;
			auto ids = ctx->stra_sell(code, targetPx, _unit, "entershort");

			_mtx_ords.lock();
			for (auto localid : ids)
			{
				_orders.insert(localid);
			}
			_mtx_ords.unlock();
			_last_entry_time = now;
		}
	}
}
void WtHftStraOrderBook::on_bar(IHftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar)
{

}
void WtHftStraOrderBook::check_orders()
{
	if (!_orders.empty() && _last_entry_time != UINT64_MAX)
	{
		uint64_t now = TimeUtils::makeTime(_ctx->stra_get_date(), _ctx->stra_get_time() * 100000 + _ctx->stra_get_secs());
		if (now - _last_entry_time >= _secs * 1000)	//如果超过一定时间没有成交完,则撤销
		{
			_mtx_ords.lock();
			for (auto localid : _orders)
			{
				_ctx->stra_cancel(localid);
				_cancel_cnt++;
				_ctx->stra_log_info(fmt::format("Order expired, cancelcnt updated to {}", _cancel_cnt).c_str());
			}
			_mtx_ords.unlock();
		}
	}
}

void WtHftStraOrderBook::on_trade(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double qty, double price, const char* userTag)
{
	
}

void WtHftStraOrderBook::on_position(IHftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)
{
	
}

void WtHftStraOrderBook::on_order(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	//如果不是我发出去的订单,我就不管了
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return;

	//如果已撤销或者剩余数量为0,则清除掉原有的id记录
	if(isCanceled || leftQty == 0)
	{
		_mtx_ords.lock();
		_orders.erase(it);
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->stra_log_info(fmt::format("cancelcnt -> {}", _cancel_cnt).c_str());
		}
		_mtx_ords.unlock();
	}
}

void WtHftStraOrderBook::on_channel_ready(IHftStraCtx* ctx)
{
	double undone = _ctx->stra_get_undone(_code.c_str());
	if (!decimal::eq(undone, 0) && _orders.empty())
	{
		//这说明有未完成单不在监控之中,先撤掉
		_ctx->stra_log_info(fmt::format("{}有不在管理中的未完成单 {} 手,全部撤销", _code, undone).c_str());

		bool isBuy = (undone > 0);
		OrderIDs ids = _ctx->stra_cancel(_code.c_str(), isBuy, undone);
		for (auto localid : ids)
		{
			_orders.insert(localid);
		}
		_cancel_cnt += ids.size();

		_ctx->stra_log_info(fmt::format("cancelcnt -> {}", _cancel_cnt).c_str());
	}

	_channel_ready = true;
}

void WtHftStraOrderBook::on_channel_lost(IHftStraCtx* ctx)
{
	_channel_ready = false;
}

int WtHftStraOrderBook::benchInstDir()
{
	if (distributionOverTime.size() < (imbalanceLevel + 1)) return 0;

	double bidNum = 0.0, askNum = 0.0;
	int imbalance = 0, lastImbalance = 0, cumulateImbalance = 0;
	for (auto it = distributionOverTime.begin(); it != distributionOverTime.end(); it++)
	{
		askNum = it->second[1];
		lastImbalance = imbalance;
		if (it != distributionOverTime.begin() && (!decimal::eq(bidNum, 0.0) || !decimal::eq(askNum, 0.0)))
		{
			if (!decimal::eq(bidNum * askNum, 0.0))
			{
				if (bidNum / askNum >= imbalanceRatio) imbalance = 1;
				else if (askNum / bidNum >= imbalanceRatio) imbalance = -1;
				else imbalance = 0;
			}
			else if (decimal::eq(askNum, 0))
			{
				imbalance = 1;
			}
			else if (decimal::eq(bidNum, 0))
			{
				imbalance = -1;
			}
		}
		else
		{
			imbalance = 0;
		}

		if (cumulateImbalance == 0)
		{
			cumulateImbalance = imbalance;
		}
		else if (lastImbalance * imbalance > 0)
		{
			cumulateImbalance += imbalance;
			if (cumulateImbalance >= imbalanceLevel) return 1;
			else if (cumulateImbalance >= -imbalanceLevel) return -1;
		}
		else
		{
			cumulateImbalance = 0;
		}

		bidNum = it->second[0];
	}
	if (cumulateImbalance >= imbalanceLevel) return 1;
	else if (cumulateImbalance >= -imbalanceLevel) return -1;
	else return 0;
}

void WtHftStraOrderBook::updateDistribution(WTSTickStruct& marketData)
{
	if (marketData.volume == 0) return;
	int activeSell = 0, activeBuy = 0;
	if (marketData.bid_prices[0] < benchLasttick.bid_prices[0])
	{
		activeSell = marketData.bid_qty[0];
	}
	else if (decimal::eq(marketData.bid_prices[0], benchLasttick.bid_prices[0]))
	{
		int delta = benchLasttick.bid_qty[0] - marketData.bid_qty[0];
		if (delta >= 0)
		{
			activeSell = delta;
		}
		else
		{
			activeSell = 0;
			//			APP_INFO("activeSell < 0!" << " delta:" << delta);
		}
	}
	else
	{
		activeSell = 0;
	}

	if (marketData.ask_prices[0] > benchLasttick.ask_prices[0])
	{
		activeBuy = benchLasttick.ask_qty[0];
	}
	else if (decimal::eq(marketData.ask_prices[0], benchLasttick.ask_prices[0]))
	{
		int delta = benchLasttick.ask_qty[0] - marketData.ask_qty[0];
		if (delta >= 0)
		{
			activeBuy = delta;
		}
		else
		{
			activeBuy = 0;
			//			APP_INFO("activeBuy < 0!" << " delta:" << delta);
		}
	}
	else
	{
		activeBuy = 0;
	}

	auto activeNum = activeSell + activeBuy;
	if (activeNum > marketData.volume || activeNum == 0)//market has cancel order
	{
		return;
	}
	double adActiveSell = (activeNum / (marketData.volume*1.0))* activeSell;
	double adActiveBuy = (activeNum / (marketData.volume*1.0))* activeBuy;

	int pricekey = (marketData.price * 10000 * 2 + 1) / 2;

	distributionOverTime[pricekey][0] += adActiveBuy;
	distributionOverTime[pricekey][1] += adActiveSell;
}

