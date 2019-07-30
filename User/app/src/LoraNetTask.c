#include "includes.h"


OS_STK AppTaskLoraPingStk[APP_TASK_LoraPing_STK_SIZE];
OS_STK AppTaskLoraPollingStk[APP_TASK_LoraPolling_STK_SIZE];
OS_STK AppTaskLoraBeaconStk[APP_TASK_LoraBeacon_STK_SIZE];


static uint16_t  LoraSlotTimeTick= 0;//lora时隙滴答
static uint16_t  LoraSlotTimeNum = 0;//时隙编号

uint32_t Lora_Frequency_PollingLink[Lora_Max_Channels]={0};  //polling信道 
uint32_t Lora_Frequency_CommLink[Lora_Max_Channels]={0};     //通信信道
uint32_t Lora_Frequency_PingLink[Lora_Max_Channels]={0};     //时隙唤醒信道

Str_Lora LoraRx;




void PrintSlotTime(void)
{
  printf ("Tick %d\n\r",LoraSlotTimeTick);
}

void Set_Slot_Tick(uint16_t dat)
{
  LoraSlotTimeTick = dat;
}

uint16_t Get_Slot_Tick(void)
{
  return(LoraSlotTimeTick);
}

/*******************************************************************************
** Function name:       Lora_Network_Sys_Init
** Descriptions:        无线网络上电初始化 
**                      1：侦听beacon----则立刻启动任务（正常复位）
**                      2：信道遍历------则延迟六小时启动任务（正常复位）
**                      3：其他状态------则延迟3小时启动任务（异常复位）
** input parameters:    void 
** output parameters:   void
** Returned value:      void 
** Created by:          程卫玺
** Created Date:        2018-5-17   
*******************************************************************************/
void Lora_Network_Init(void)
{
  /********************初始化信道*******************/
   for(uint8_t num=0;num<Lora_Max_Channels;num++)
  {
    Lora_Frequency_PollingLink[num]     = UpLink_Frequency_Start   + num * 600000; //  470300000 80---无线上行信道 起始编号
    Lora_Frequency_CommLink[num]        = Comm_Frequency_Start     + num * 600000; //  470300000 80---无线上行信道 起始编号
    Lora_Frequency_PingLink[num]        = DownLink_Frequency_Start + num * 600000; //  500300000
  }
}

/*
时隙号 算唤醒点 
 0                      0.5                       1                                                     2     时间（秒）
             0                       1                          2                        3                    时隙号
 0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18  19  20  21 22 23 24 25 26 27  28  29  30  31  32   时隙滴答

 0  1  2  3  4  5  6  7  8  9  10  11  12  13  14  15  16  17  18  19  20  21  22  23  时间（点） 
   
 0      点 日冻结数据轮抄开始
 4      点 广播校时  心跳保持
 8      点 广播校时  心跳保持
 12     点 广播校时  心跳保持
 16     点 广播校时  心跳保持
 20     点 广播校时  心跳保持 
 23: 20 点 网关复位  抄表暂停
 23: 25 点 节点复位  抄表暂停  错过节点入网时间（早于） 
 23: 26 --- 00:00 查询时隙或者重新入网时间
*/
void Lora_SlotTime_Network(void)
{
  if( ++(LoraSlotTimeTick) >= 5760 ) //累加时隙滴答 360秒 
  {
    LoraSlotTimeTick = 0;
  }
  
  if(LoraSlotTimeTick % 160 == 0)//10秒钟打印一次 
  {
    PrintSlotTime();
  }

  if(LoraSlotTimeTick % 12 == 0)//查找时隙开始点  5640
  {
    LoraSlotTimeNum = LoraSlotTimeTick / 12;// 计算时隙号
    if(LoraSlotTimeNum < MeterLibMaxNum)// 表库范围内进行查询 460
    {
      if( R_State(&SysState,LoraComming) == false )// 判断无线是否正在工作 如果空闲就去申请抄表 
      {
        if( Get_Meter_Lib_Data_State(LoraSlotTimeNum) == true )// 查找本时隙号下是否有任务 
        {
          OSMboxPost(MboxLoraPing, &LoraSlotTimeNum);//    调用抄表程序 传递时隙号 
          OSTaskSuspend(APP_TASK_LoraPolling_PRIO);//挂起lora polling任务 
        }
      }
    }
    else
    {
      if( LoraSlotTimeNum == LoraSlotTimeBSNum)//从470号时隙进行广播 
      {  
        OSMboxPost(MboxLoraPing, &LoraSlotTimeNum);//    调用抄表程序 传递时隙号 
        OSTaskSuspend(APP_TASK_LoraPolling_PRIO);//挂起lora polling任务 
      }
    }
  }
}



void AppTaskLoraPing(void *p_arg)
{
  static uint16_t LoraPingDelay = 0;

  (void)p_arg;		/* 避免编译器告警 */
  
  while (1) 
  {
    LoraPingDelay = LoraPingTask();//
    OSTimeDly(LoraPingDelay);
  }
}
 

uint16_t LoraPingTask(void)
{
  static INT8U   err;
  static uint16_t stn = 0;
  static ENUM_Ping_StateMachine PingStateMachine;
  static uint8_t PingTxDataNum = 0;//集中器点抄节点此时 
  uint32_t delay = 0;
  Str_Lora tx;
  uint8_t id[6]={0};
  Union_Lora_Protocol_MSG_Q msg;
  
  switch( PingStateMachine )
  {
  case Enum_Ping_Enable: // 打开接收 等待节点polling类型数据 
    { 
      PingTxDataNum = 0;
      OSTaskResume(APP_TASK_LoraPolling_PRIO);                           //挂起lora polling任务 
      W_State(&SysState,LoraComming,false);
      stn = *(INT16U *)(OSMboxPend(MboxLoraPing, 0 , &err));             //等待消息邮箱 温度信息
      W_State(&SysState,LoraComming,true);

      if(stn == LoraSlotTimeBSNum)//如果是广播时隙 
      {
        Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_PingLink[ ConfigInfo.LoraChannel ]);//在ping信道发送唤醒数据帧
        msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
        msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode = NetProtocol_BroadcastTime;
        msg.Str_Lora_Protocol_MSG_Q.SlotTick = Get_Slot_Tick();
        Combine_LoRa_Protocol_Frame(&tx,&msg);
        Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,Sx1278_Calculated_Cad_Preamble(2000));//前导码 2秒
        delay = 0;
        PingStateMachine = Enum_Ping_Tx_PingType;
      }
      else                        //如果是普通时隙
      {
        Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_PingLink[ ConfigInfo.LoraChannel ]);//在ping信道发送唤醒数据帧
        msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
        msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode = NetProtocol_PingType;
        Get_Meter_Lib_ID(stn,&msg.Str_Lora_Protocol_MSG_Q.NodeID[0],4);
        Combine_LoRa_Protocol_Frame(&tx,&msg);
        Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,Sx1278_Calculated_Cad_Preamble(750));//前导码 750ms
        delay = 0;
        PingStateMachine = Enum_Ping_Tx_PingType;
      }
    }
    break;
  case Enum_Ping_Tx_PingType:     
    {
      OSSemPend(SempLoraTx, 5000, &err);//等待发送完成 
      
      if( err != OS_ERR_TIMEOUT )//不是超时的话
      {
        if(stn == LoraSlotTimeBSNum)//广播时隙 发送完成就回到初始化 
        {
          delay = 0;
          PingStateMachine = Enum_Ping_Enable;
        }
        else                        //如果点抄  就跳入到下一个状态 打开接收 
        {
          delay = 0;
          PingStateMachine = Enum_Ping_Rx_PingType;
        }
      }
      else
      {
        delay = 0;
        PingStateMachine = Enum_Ping_Enable;
      }
    }
    break;
  case Enum_Ping_Rx_PingType://等待接收 pingtype应答 
    {
      Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //信道
      Sx1278_LoRa_Set_RxMode( 0 ); //启动连续接收
      
      OSSemPend(SempLoraRx, 5000, &err);

      if( err != OS_ERR_TIMEOUT )//不是超时的话
      {
        Resolve_LoRa_Protocol_Frame(&LoraRx.buf[0] , LoraRx.len ,&msg);
        if(msg.Str_Lora_Protocol_MSG_Q.Validity == true)//判断 数据帧有效性 
        {
          if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode == NetProtocol_PingType)//判断 数据帧类型对不对 
          {
            Get_Meter_Lib_ID(stn,&id[0],4);
            if( memcmp(&id[0],&(msg.Str_Lora_Protocol_MSG_Q.NodeID[0]),4)==0 )//判断 应答节点ID是否正确 
            {
              delay = 0;
              PingTxDataNum = 0;
              PingStateMachine = Enum_Ping_Tx_Data;
            }
            else
            {
              delay = 0;
              PingStateMachine = Enum_Ping_Enable;
            }
          }
          else
          {
            delay = 0;
            PingStateMachine = Enum_Ping_Enable;
          }
        }
        else
        {
          delay = 0;
          PingStateMachine = Enum_Ping_Enable;
        }
      }
      else//超时没有接收到 就返回
      {
        delay = 0;
        PingStateMachine = Enum_Ping_Enable;
      }
    }
    break;
  case Enum_Ping_Tx_Data://等待接收 pingtype应答 
    {
      if(PingTxDataNum < 3)
      {
        PingTxDataNum++;
        Get_Meter_Lib_ID(stn,&id[0],4);
        memcpy(&(msg.Str_Lora_Protocol_MSG_Q.NodeID[0]),&id[0],4);
        msg.Str_Lora_Protocol_MSG_Q.BufLen = Get_Meter_Lib_Data(stn,&msg.Str_Lora_Protocol_MSG_Q.Buf[0]);
        msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
        msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode = NetProtocol_PingData; 
        Combine_LoRa_Protocol_Frame(&tx,&msg);
        Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //信道
        Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);//前导码 750ms
        
        delay = 0;
        PingStateMachine = Enum_Ping_Tx_Data_End;
      }
      else
      {
        PingTxDataNum=0;
        
        delay = 0;
        PingStateMachine = Enum_Ping_Enable;
      }
    }
    break;
  case Enum_Ping_Tx_Data_End:
    {
      OSSemPend(SempLoraTx, 5000, &err);
      
      if( err != OS_ERR_TIMEOUT )//不是超时的话
      {        
        delay = 0;
        PingStateMachine = Enum_Ping_Rx_Data;
        //发完之后马上进行接收 
      }
      else
      {
        delay = 0;
        PingStateMachine = Enum_Ping_Enable;
      }
    }
    break;
  case Enum_Ping_Rx_Data://接收ping数据  
    {
      
      Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //信道
      Sx1278_LoRa_Set_RxMode( 0 ); //启动连续接收
      
      OSSemPend(SempLoraRx, 5000, &err);
      
      if( err != OS_ERR_TIMEOUT ) 
      {
        Resolve_LoRa_Protocol_Frame(&LoraRx.buf[0] , LoraRx.len ,&msg);
        if(msg.Str_Lora_Protocol_MSG_Q.Validity == true)//判断 数据帧有效性 
        {
          if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode == NetProtocol_PingData)//判断 数据帧类型对不对 
          {
            Empty_Meter_Lib_Data(stn); //清除表库任务
            
            
            
            
            //加入4G发送队列 结束ping任务  
            delay = 0;
            PingStateMachine = Enum_Ping_Enable;
          }
          else
          {
            delay = 0;
            PingStateMachine = Enum_Ping_Tx_Data;
          }
        }
        else
        {
          delay = 0;
          PingStateMachine = Enum_Ping_Tx_Data;
        }
      }
      else                      //接收超时 
      {
        delay = 0;
        PingStateMachine = Enum_Ping_Tx_Data;//接收超时 再次发送读表命令
      }
    }
    break;
  }
  return(delay);
}


 
void AppTaskLoraPolling(void *p_arg)
{
  static uint16_t LoraPollingDelay = 0;

  (void)p_arg;		/* 避免编译器告警 */
  
  while (1) 
  {
    LoraPollingDelay = LoraPollingTask(); 
    OSTimeDly(LoraPollingDelay);
  }
}



uint16_t LoraPollingTask(void)
{
  static ENUM_Polling_StateMachine PollingStateMachine = Enum_Polling_Rx_PollingType;
  static Union_Lora_Protocol_MSG_Q msg;
  INT8U  err;
  static uint8_t TxDataLoop=0;
  Str_Calendar rtc;
  uint16_t delay = 0;
  Str_Lora tx;
  uint16_t stn;
  
  switch(PollingStateMachine)
  {
  case Enum_Polling_Rx_PollingType: // 打开接收 等待节点PollingType数据 
    { 
      LoraRx.len = 0;
 
      Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_PollingLink[ConfigInfo.LoraChannel]); //信道
      printf("PollingLink:%d\n", Lora_Frequency_PollingLink[ConfigInfo.LoraChannel]);
      Sx1278_LoRa_Set_RxMode( 0 ); //启动连续接收
      
      W_State(&SysState,LoraComming,false);
      printf("**Polling等待接收Polling类型数据**\n\r");
      
      OSSemPend(SempLoraRx, 0, &err);
      
      W_State(&SysState,LoraComming,true);
      
      Resolve_LoRa_Protocol_Frame(&LoraRx.buf[0] , LoraRx.len ,&msg);
      
      if(msg.Str_Lora_Protocol_MSG_Q.Validity == true)//有效数据帧 
      {
        if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode == NetProtocol_PollingType)//polling类型
        {
          printf("**Polling发送Polling类型数据**\n\r");
          Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //信道
          printf("CommLink:%d\n", Lora_Frequency_CommLink[ConfigInfo.LoraChannel]);

          Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
          Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
          delay = 0;
          PollingStateMachine = Enum_Polling_Tx_PollingType;//等待接收到数据 
        }
        else
        {
          delay = 0;
          PollingStateMachine = Enum_Polling_Rx_PollingType;
        }
      }
      else
      {
        delay = 0;
        PollingStateMachine = Enum_Polling_Rx_PollingType;
      }
    }
    break;
  case Enum_Polling_Tx_PollingType://应答完毕之后马上打开接收 继续等待节点的PollingData
    {
      OSSemPend(SempLoraTx, 5000, &err);
      
      if( err != OS_ERR_TIMEOUT )//不是超时的话
      {
        delay = 0;
        PollingStateMachine = Enum_Polling_Rx_PollingData;//马上进入接收模式 等待节点的 pollingdata
      }
      else
      {
        printf("**Polling发送完成Polling类型超时 **\n\r");
        delay = 0;
        PollingStateMachine = Enum_Polling_Rx_PollingType;
      }
    }
    break;
  case Enum_Polling_Rx_PollingData://应答完毕之后马上打开接收 继续等待节点的PollingData
    {
      printf("**Polling发送完成Polling类型数据**\n\r");
      Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //打开通信信道接收pollingDATA
      Sx1278_LoRa_Set_RxMode( 1 ); //启动连续接收
      
      OSSemPend(SempLoraRx, 5000, &err);
      
      RS8025T_Get_Calendar_Time(&rtc); 
      
      if( err != OS_ERR_TIMEOUT )//不是超时的话
      {
        Resolve_LoRa_Protocol_Frame(&LoraRx.buf[0] , LoraRx.len ,&msg);
        if(msg.Str_Lora_Protocol_MSG_Q.Validity == true)
        {
          if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode      == NetProtocol_QueryGateway)   //查询网关
          {
            msg.Str_Lora_Protocol_MSG_Q.SignalStrength = LoraRx.rssi;
            Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
            Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
            delay = 0;
            PollingStateMachine = Enum_Polling_Tx_PollingData;
          }
          else if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode == NetProtocol_GateWayRegister)//入网请求
          {
            if( Seek_Lib_ID(&msg.Str_Lora_Protocol_MSG_Q.NodeID[0],&stn) == true )//这块表占用过一个时隙  需要把之前的时隙清除掉 
            {
              Empty_Meter_Lib_ID(stn);
            }
            if(Insert_Meter_Lib(&msg.Str_Lora_Protocol_MSG_Q.NodeID[0],&stn) == true)//成功插入表库 
            { 
              printf("插入表库成功 (%d)\r\n", stn);
              msg.Str_Lora_Protocol_MSG_Q.BufLen     = 0x0a;
              msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
              msg.Str_Lora_Protocol_MSG_Q.SlotTick   = Get_Slot_Tick() ;
              msg.Str_Lora_Protocol_MSG_Q.Rtc        = rtc;
              msg.Str_Lora_Protocol_MSG_Q.SlotNum    = stn;
              Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
              Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
              delay = 0;
              PollingStateMachine = Enum_Polling_Tx_PollingData;
            }
            else                                                                      //插入表库失败 
            {
              msg.Str_Lora_Protocol_MSG_Q.BufLen     = 0x00;
              msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
              msg.Str_Lora_Protocol_MSG_Q.SlotTick   = 0;
              msg.Str_Lora_Protocol_MSG_Q.Rtc        = rtc;
              msg.Str_Lora_Protocol_MSG_Q.SlotNum    = 0x01;
              Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
              Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
              delay = 0;
              PollingStateMachine = Enum_Polling_Tx_PollingData;
            }
          }
          else if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode == NetProtocol_QuerySlot)			//失步查询
          {
            msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
            msg.Str_Lora_Protocol_MSG_Q.SlotTick   = Get_Slot_Tick();
            msg.Str_Lora_Protocol_MSG_Q.Rtc        = rtc;
            msg.Str_Lora_Protocol_MSG_Q.SlotNum    = 0x01;
            Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
            Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
            delay = 0;
            PollingStateMachine = Enum_Polling_Tx_PollingData;
          }
          else if(msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode == NetProtocol_PollingData)		//polling数据
          {
            
            msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
            Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
       		Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
            delay = 0;
            PollingStateMachine = Enum_Polling_Tx_PollingData;
          }
          else
          {
            Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //打开通信信道接收pollingDATA
            Sx1278_LoRa_Set_RxMode( 1 ); //启动连续接收
            delay = 0;
            PollingStateMachine = Enum_Polling_Tx_PollingData;
          }
        }
        else
        {
          Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_CommLink[ConfigInfo.LoraChannel]); //打开通信信道接收pollingDATA
          Sx1278_LoRa_Set_RxMode( 1 ); //启动连续接收
          delay = 0;
          PollingStateMachine = Enum_Polling_Tx_PollingData;
        }
      }
      else
      {
        if(++TxDataLoop >= 2)
        {
          TxDataLoop = 0;
          printf("**Polling接收Polling数据超时 **\n\r");
          delay = 0;
          PollingStateMachine = Enum_Polling_Rx_PollingType; 
        }
        else
        {
          delay = 0;
          PollingStateMachine = Enum_Polling_Rx_PollingData;
        }
      }
    }
    break;
  case Enum_Polling_Tx_PollingData://应答完毕之后马上打开接收 继续等待节点的PollingData
    {
      OSSemPend(SempLoraTx, 5000, &err);
      
      printf("**Polling发送完成Polling类型数据**\n\r");
      delay = 0;
      PollingStateMachine = Enum_Polling_Rx_PollingData;
    }
   break;
  }
  return(delay);
}



void AppTaskLoraBeacon(void *p_arg)
{
  static INT8U  err;
  (void)p_arg;		/* 避免编译器告警 */
  
  while (1) 
  {
    Union_Lora_Protocol_MSG_Q msg;
    Str_Calendar rtc;
    Str_Lora tx;
    
    RS8025T_Get_Calendar_Time(&rtc);
    
    if(rtc.Hours == 0x23)
    {
      if((rtc.Minutes >= 0x35)&&(rtc.Minutes <= 0x55))
      {
        msg.Str_Lora_Protocol_MSG_Q.ControlCode.Str_LoRa_Ctrl.FunctionCode = NetProtocol_Beacon;
        msg.Str_Lora_Protocol_MSG_Q.ChannleNum = ConfigInfo.LoraChannel;
        msg.Str_Lora_Protocol_MSG_Q.SlotTick   = Get_Slot_Tick();
        msg.Str_Lora_Protocol_MSG_Q.Rtc        = rtc;
        
        W_State(&SysState,LoraSendOK,false); //清除标志 
        Sx1278_LoRa_Set_RFFrequency(Lora_Frequency_PingLink[ConfigInfo.LoraChannel]); //打开通信信道接收pollingDATA
        Combine_LoRa_Protocol_Frame(&tx,&msg);//发送应答PollingType数据 
        Sx1278_Lora_Tx_Data(&tx.buf[0],tx.len,12);
        
        OSSemPend(SempLoraTx, 2000, &err);
        
        OSTimeDly(2000);//3秒发送一次
      }
      else
      {
        OSTaskSuspend(APP_TASK_LoraBeacon_PRIO);
      }
    }
    else
    {
      OSTaskSuspend(APP_TASK_LoraBeacon_PRIO);
    }
  }
}
 