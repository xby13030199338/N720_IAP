/* CH376оƬ Ӳ������� V1.0 */
/* �ṩ�����ӳ��� */

#include	"HAL.H"

/* ��ʱָ��΢��ʱ��,���ݵ�Ƭ����Ƶ����,����ȷ */
void	mDelayuS( UINT8 us )
{
	int i;

	for (i = 0; i < 6 * us; i++);
}

/* ��ʱָ������ʱ��,���ݵ�Ƭ����Ƶ����,����ȷ */
void mDelaymS( UINT8 ms )
{
	while ( ms -- ) {
		mDelayuS( 250 );
		mDelayuS( 250 );
		mDelayuS( 250 );
		mDelayuS( 250 );
	}
}