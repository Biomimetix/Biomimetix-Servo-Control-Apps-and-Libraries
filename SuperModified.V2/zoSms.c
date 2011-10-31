#include <avr/io.h>
#include "zoSms.h"
#include "zoMotor.h"
#include <avr/eeprom.h>
#include <math.h>
#include "zoMcu.h"
#include "zoCommands.h"
#include "zoSmsError.h"
#include "zoAdc.h"

struct ZO_SUPERMODIFIED Sms = 
		{ {ZO_SMS_CONTROL_NO_CONTROL,0.0},
		 {0,0,0,0,0,0,0,0},
		 {0.0,0.0,0.0,0.0,0,0,0,0,0,ZO_SMS_PROFILE_ACCELERATING},
		 ZO_PID_DEFAULTS,
		 {ZO_SMS_SETPOINT_NONE,0}
		};


void zoSmsControlLoop(void)
{
	zoEncoderSetVel();
	
	if(Sms.Control.State == ZO_SMS_CONTROL_NO_CONTROL)
		return;

	switch (Sms.Control.State)
	{
		case ZO_SMS_CONTROL_POSITION_CONTROL:
			break;

		case ZO_SMS_CONTROL_VELOCITY_CONTROL:
			Sms.Profile.PositionSetpoint += Sms.Profile.VelocityDeltaPos;
			Sms.Pid.SetPoint = (s32)Sms.Profile.PositionSetpoint;  
			break;

		case ZO_SMS_CONTROL_PROFILED_VELOCITY_CONTROL:
			if( !accelerationCompleteCriterion() )
			{
				Sms.Profile.VelocityDeltaPos += Sms.Profile.ConstAccelDeltaVel;
				Sms.Profile.PositionSetpoint +=	Sms.Profile.VelocityDeltaPos;					
				Sms.Pid.SetPoint = (s32)Sms.Profile.PositionSetpoint;
			}
			else
			{
				Sms.Control.State  = ZO_SMS_CONTROL_VELOCITY_CONTROL;
				Sms.Profile.VelocityDeltaPos = Sms.Profile.TargetVelDeltaPos;
			}
			break;

		case ZO_SMS_CONTROL_PROFILED_POSITION_CONTROL:
			switch(Sms.Profile.State)
			{
				case ZO_SMS_PROFILE_ACCELERATING:
					if( !accelerationCompleteCriterion() )
					{
						Sms.Profile.VelocityDeltaPos += Sms.Profile.ConstAccelDeltaVel;
						Sms.Profile.PositionSetpoint +=	Sms.Profile.VelocityDeltaPos;					
						Sms.Pid.SetPoint = (s32)Sms.Profile.PositionSetpoint;
					}
					else
					{
						Sms.Profile.State  = ZO_SMS_PROFILE_MOVING_WITH_VELOCITY;
					}
					break;

				case ZO_SMS_PROFILE_MOVING_WITH_VELOCITY:
					if( !constantVelocityCompletedCriterion() )
					{
						Sms.Profile.PositionSetpoint += Sms.Profile.TargetVelDeltaPos;
						Sms.Pid.SetPoint = (s32)Sms.Profile.PositionSetpoint;
					}
					else
						Sms.Profile.State = ZO_SMS_PROFILE_DECCELERATING;
					break;

				case ZO_SMS_PROFILE_DECCELERATING:
					if( !positionReachedCriterion() )
					{
						Sms.Profile.VelocityDeltaPos -= Sms.Profile.ConstAccelDeltaVel;//deccelRecalc();
						Sms.Profile.PositionSetpoint +=	Sms.Profile.VelocityDeltaPos;
						Sms.Pid.SetPoint = (s32)Sms.Profile.PositionSetpoint;
					}
					else
					{
						Sms.Profile.PositionSetpoint = Sms.Profile.TargetPosition;
						Sms.Pid.SetPoint = Sms.Profile.TargetPosition;
						Sms.Control.State = ZO_SMS_CONTROL_POSITION_CONTROL;
					}
					
			}
			break;

			case ZO_SMS_CONTROL_NO_CONTROL:	//Prevent compiler warning
				break;
	}

	Sms.Pid.doPID(&Sms.Pid);
}

void zoSmsInit(void)
{
	//initialize MCU pins
	zoSmsPinsInit();

	//initialize motor. Also responsible for control loop timing.
	zoMotorInit();
	zoMotorSetState(FALSE);
	cli();	//disable interrupts during initialization

	//attach the control loop handler
	zoMotorSetTimedEventHandler(zoSmsControlLoop);

	//initialize the encoder
	zoEncoderInit();
	zoEncoderReset();
	cli();

	//get all settings from eeprom
	zoSmsInitSettingsFromEeprom();
	
	//initialize other variables
	Sms.BufferdSetPoint.type = ZO_SMS_SETPOINT_NONE;
	Sms.Pid.Error = 0;
	Sms.Pid.PrevError = 0;
	Sms.Pid.Integral = 0;
	Sms.Pid.SetPoint = 0;
	Sms.Pid.setActuator = zoMotorSet;
	Sms.Pid.getFeedBack = zoEncoderGetPos;
	Sms.Pid.AntiWindUp = 0xFFFF/(Sms.Pid.GainP)*0xFF;
	Sms.Profile.VelocityDeltaPos = 0;
	Sms.Control.State = ZO_SMS_CONTROL_NO_CONTROL;
	
	zoSmsErrorInit();

	//TODO: think of another ADC scheme that takes less CPU power
	//now the adcisr runs at ~10KHz
	zoAdcInit();
	cli();
	zoAdcSetTrigger(ZO_ADC_TRIGGER_FREE_RUNNING);
	zoAdcChannelEnable(ZO_ADC_CHANNEL_0);
	zoAdcChannelEnable(ZO_ADC_CHANNEL_1);
	zoAdcChannelEnable(ZO_ADC_CHANNEL_2);
	zoAdcChannelEnable(ZO_ADC_CHANNEL_3);
	zoAdcChannelEnable(ZO_ADC_CHANNEL_7);
	
	//initialize global interrupts -> go
	sei();
}

void zoSmsPinsInit(void)
{
	//PORTB pin assignments_______________________________________________________________
	//PB0: DIO2		: Digitl IO 2 initialized as input
	//PB1: DIO3		: Digitl IO 3 initialized as input
	//PB2: SS		: SPI chip select
	//PB3: MOSI		: SPI Master out slave in
	//PB4: MISO		: SPI Master in slave out
	//PB5: SCK		: SPI clock
	
	DDRB = 0x04;
	PORTB |= 0x3E;	

	//PORTC pin assignments_______________________________________________________________
	//PC0: AIN1		: Analog input 1
	//PC1: AIN2		: Analog input 2
	//PC2: AIN3		: Analog input 3
	//PC3: AIN4		: Analog input 4
	//PC4: SDA		: I2C data, configured as input, pull-up enabled
	//PC5: SCL		: I2C clock, configured as input, pull-up enabled
	//PC6: NOT USED
	//PC7: NOT USED

	DDRC = 0x00;
	PORTC |= 0x30;

	//PORTD pin assignments_______________________________________________________________
	//PD0: R_485	: Uart receive, input, pull-up enabled
	//PD1: T_485	: Uart transmit, configured as input for high impedance to allow other nodes on the bus
	//PD2: R/D_485	: 485 Mode configuration pin: 1->Tx,0->Rx, output, default set to Rx
	//PD3: PWM2		: PWM2 output to motor driver
	//PD4: MOT_EN	: motor enable output to motor driver: 0->disabled, 1->enabled
	//PD5: PWM1		: PWM2 output to motor driver
	//PD6: DIO4		: Digital IO4, initially configured as input
	//PD7: DIO1		: Digital IO1, initially configured as input

	DDRD = 0x3C;
	PORTD = 0xC0;
}


void zoSmsInitSettingsFromEeprom(void)
{
	Sms.Settings.NodeID = eeprom_read_byte((u08*)ZO_EEPROM_ADDRESS_NODE_ID);
	Sms.Pid.GainP = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_GAIN_P);
	Sms.Pid.GainI = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_GAIN_I);
	Sms.Pid.GainD = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_GAIN_D);
	Sms.Settings.CurrentLimit = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_CURRENT_LIMIT);
	Sms.Settings.CurrentLimitDuration = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_CURRENT_LIMIT_DURATION);
	Sms.Settings.DigitalIoConfig = eeprom_read_byte((u08*)ZO_EEPROM_ADDRESS_DIGITAL_IO_CONFIG);
	Sms.Settings.BaudUart = eeprom_read_dword((u32*)ZO_EEPROM_ADDRESS_BAUD_UART);
	Sms.Settings.BaudI2C = eeprom_read_dword((u32*)ZO_EEPROM_ADDRESS_BAUD_I2C);
	Sms.Profile.DesiredAcceleration = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_PROFILE_ACCELERATION);
	Sms.Profile.DesiredVelocity = eeprom_read_word((u16*)ZO_EEPROM_ADDRESS_PROFILE_VELOCITY);
	Sms.Settings.localAcceptanceMask = eeprom_read_byte((u08*)ZO_EEPROM_ADDRESS_LAM);
	Sms.Settings.errorReportingLevel = eeprom_read_byte((u08*)ZO_EEPROM_ADDRESS_ERROR_REPORTING_LVL);
}

void zoSmsRestoreDefaults(void)
{
	eeprom_write_byte((u08*)ZO_EEPROM_ADDRESS_NODE_ID, ZO_DEFAULT_NODE_ID);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_GAIN_P, ZO_DEFAULT_GAIN_P);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_GAIN_I, ZO_DEFAULT_GAIN_I);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_GAIN_D, ZO_DEFAULT_GAIN_D);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_CURRENT_LIMIT, ZO_DEFAULT_CURRENT_LIMIT);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_CURRENT_LIMIT_DURATION, ZO_DEFAULT_CURRENT_LIMIT_DURATION);
	eeprom_write_byte((u08*)ZO_EEPROM_ADDRESS_DIGITAL_IO_CONFIG, ZO_DEFAULT_DIGITAL_IO_CONFIG);
	eeprom_write_dword((u32*)ZO_EEPROM_ADDRESS_BAUD_UART, ZO_DEFAULT_BAUD_UART);
	eeprom_write_dword((u32*)ZO_EEPROM_ADDRESS_BAUD_I2C, ZO_DEFAULT_BAUD_I2C);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_PROFILE_ACCELERATION, ZO_DEFAULT_PROFILE_ACCELERATION);
	eeprom_write_word((u16*)ZO_EEPROM_ADDRESS_PROFILE_VELOCITY, ZO_DEFAULT_PROFILE_VELOCITY);
	eeprom_write_byte((u08*)ZO_EEPROM_ADDRESS_LAM, ZO_DEFAULT_LAM);
	eeprom_write_byte((u08*)ZO_EEPROM_ADDRESS_ERROR_REPORTING_LVL, ZO_DEFAULT_ERROR_REPORTING_LVL);
}

//double inline deccelRecalc(void)
//{
//	return(  0.5 * square(Sms.Profile.VelocityDeltaPos) / 
//		((double)(Sms.Profile.TargetPosition - zoEncoderGetStoredPos())));
//}

bool inline accelerationCompleteCriterion(void)
{
	if( labs( (s32)(Sms.Profile.VelocityDeltaPos*Sms.Control.LoopHz) - (s32)(Sms.Profile.TargetVelDeltaPos*Sms.Control.LoopHz) ) < 2 )
		return TRUE;
	else
		return FALSE;
}

bool inline constantVelocityCompletedCriterion(void)
{
	return (( labs(zoEncoderGetStoredPos() - Sms.Profile.DeccelerationKnee) >= 2 )?FALSE:TRUE);
}

bool inline positionReachedCriterion(void)
{
	if( (( labs( (s32)(Sms.Profile.VelocityDeltaPos*Sms.Control.LoopHz) ) ) < 2 ) || 
		( labs(zoEncoderGetStoredPos() - Sms.Profile.TargetPosition) < 2 ) )
		return TRUE;
	else
		return FALSE;
}



