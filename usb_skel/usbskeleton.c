/*
 * USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c
 * but has been rewritten to be easier to read and use.
 *
 */


#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/input-polldev.h>
#include <linux/usb/input.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Benoit Juin <benoit@aeon-creation.com>");
MODULE_DESCRIPTION("GreenTouch touch foil device driver");
MODULE_LICENSE("GPL");


/* Define these values to match your devices */
#define USB_SKEL_VENDOR_ID	0x0547
#define USB_SKEL_PRODUCT_ID	0x2001 

#define POLL_INTERVAL 10
#define NAME_LONG "GreenTouch MT" 

/* sensor resolution */
#define SENSOR_RES_X 1920
#define SENSOR_RES_Y 1080
#define MAX_CONTACTS 10
#define SIGMA_THRESHOLD 275
#define SIGMA_COMPUTE_FRAME 255
#define AVERAGE_COMPUTE_FRAME 255
#define CALIBRATE_EVERY 7000
#define BLOB_LINE_OFFSET 0

/* table of devices that work with this driver */
static const struct usb_device_id skel_table[] = {
	{ USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, skel_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8
/* arbitrarily chosen */
struct touch_contact {
  int x;
  int y;
  int h;
  int w;
  bool processed;
};


/* Structure to hold all of our device specific stuff */
struct usb_skel {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
        unsigned short          *score_frame;	        /* store the first frame */
        unsigned short          *score_frame_adjacent;	        /* store the first frame */
        unsigned short          *score_last_frame_adjacent;	        /* store the first frame */
    	unsigned short          *sigma_frame;/* store the first frame */
        unsigned short          *average_frame;	        /* store the first frame */
	bool			sigma_normalized;	/* a read is going on */
        bool			average_computed;
        int                     frame_index;
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	int			errors;			/* the last request tanked */
	bool			ongoing_read;		/* a read is going on */
  
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	wait_queue_head_t	bulk_in_wait;		/* to wait for an ongoing read */
        char phys[64];
        struct input_polled_dev *input;
        struct touch_contact touch_contacts[MAX_CONTACTS];
};
#define to_skel_dev(d) container_of(d, struct usb_skel, kref)

static struct usb_driver skel_driver;
static void skel_draw_down(struct usb_skel *dev);

static void skel_delete(struct kref *kref)
{
	struct usb_skel *dev = to_skel_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

static int skel_open(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&skel_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = dev;

exit:
	return retval;
}

static int skel_release(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* allow the device to be autosuspended */
	mutex_lock(&dev->io_mutex);
	if (dev->interface)
		usb_autopm_put_interface(dev->interface);
	mutex_unlock(&dev->io_mutex);

	/* decrement the count on our device */
	kref_put(&dev->kref, skel_delete);
	return 0;
}

static int skel_flush(struct file *file, fl_owner_t id)
{
	struct usb_skel *dev;
	int res;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* wait for io to stop */
	mutex_lock(&dev->io_mutex);
	skel_draw_down(dev);

	/* read out errors, leave subsequent opens a clean slate */
	spin_lock_irq(&dev->err_lock);
	res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);

	mutex_unlock(&dev->io_mutex);

	return res;
}

static void skel_read_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev;
	printk("%s\n", __func__);
	dev = urb->context;

	spin_lock(&dev->err_lock);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock(&dev->err_lock);

	wake_up_interruptible(&dev->bulk_in_wait);
}

static int skel_do_read_io(struct usb_skel *dev, size_t count)
{
	int rv;
	
  	printk("%s\n", __func__);
	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			skel_read_bulk_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = 1;
	spin_unlock_irq(&dev->err_lock);

	/* submit bulk in urb, which means no data to deliver */
	dev->bulk_in_filled = 0;
	dev->bulk_in_copied = 0;

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = 0;
		spin_unlock_irq(&dev->err_lock);
	}

	return rv;
}

static ssize_t skel_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct usb_skel *dev;
	int rv;
	bool ongoing_io;

	dev = file->private_data;

  	printk("%s\n", __func__);
	
	/* if we cannot read at all, return EOF */
	if (!dev->bulk_in_urb || !count)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	if (!dev->interface) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}

	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irq(&dev->err_lock);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irq(&dev->err_lock);

	if (ongoing_io) {
		/* nonblocking IO shall not wait */
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}
		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		rv = wait_event_interruptible(dev->bulk_in_wait, (!dev->ongoing_read));
		if (rv < 0)
			goto exit;
	}

	/* errors must be reported */
	rv = dev->errors;
	if (rv < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* report it */
		goto exit;
	}

	/*
	 * if the buffer is filled we may satisfy the read
	 * else we need to start IO
	 */

	if (dev->bulk_in_filled) {
		/* we had read data */
		size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
		size_t chunk = min(available, count);

		if (!available) {
			/*
			 * all data has been used
			 * actual IO needs to be done
			 */
			rv = skel_do_read_io(dev, count);
			if (rv < 0)
				goto exit;
			else
				goto retry;
		}
		/*
		 * data is available
		 * chunk tells us how much shall be copied
		 */

		if (copy_to_user(buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		dev->bulk_in_copied += chunk;

		/*
		 * if we are asked for more than we have,
		 * we start IO but don't wait
		 */
		if (available < count)
			skel_do_read_io(dev, count - chunk);
	} else {
		/* no data in the buffer */
		rv = skel_do_read_io(dev, count);
		if (rv < 0)
			goto exit;
		else
			goto retry;
	}
exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

static void skel_write_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev;

	dev = urb->context;

	printk("%s\n", __func__);

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		spin_lock(&dev->err_lock);
		dev->errors = urb->status;
		spin_unlock(&dev->err_lock);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}

static ssize_t skel_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	struct usb_skel *dev;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min(count, (size_t)MAX_TRANSFER);

	dev = file->private_data;

	printk("%s\n", __func__);

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/*
	 * limit the number of URBs in flight to stop a user from using up all
	 * RAM
	 */
	if (!(file->f_flags & O_NONBLOCK)) {
		if (down_interruptible(&dev->limit_sem)) {
			retval = -ERESTARTSYS;
			goto exit;
		}
	} else {
		if (down_trylock(&dev->limit_sem)) {
			retval = -EAGAIN;
			goto exit;
		}
	}

	spin_lock_irq(&dev->err_lock);
	retval = dev->errors;
	if (retval < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irq(&dev->err_lock);
	if (retval < 0)
		goto error;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}

	if (copy_from_user(buf, user_buffer, writesize)) {
		retval = -EFAULT;
		goto error;
	}

	/* this lock makes sure we don't submit URBs to gone devices */
	mutex_lock(&dev->io_mutex);
	if (!dev->interface) {		/* disconnect() was called */
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  buf, writesize, skel_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &dev->submitted);

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting write urb, error %d\n",
			__func__, retval);
		goto error_unanchor;
	}

	/*
	 * release our reference to this urb, the USB core will eventually free
	 * it entirely
	 */
	usb_free_urb(urb);


	return writesize;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&dev->limit_sem);

exit:
	return retval;
}

static const struct file_operations skel_fops = {
	.owner =	THIS_MODULE,
	.read =		skel_read,
	.write =	skel_write,
	.open =		skel_open,
	.release =	skel_release,
	.flush =	skel_flush,
	.llseek =	noop_llseek,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver skel_class = {
	.name =		"skel%d",
	.fops =		&skel_fops,
	.minor_base =	USB_SKEL_MINOR_BASE,
};



void debug_matrix(unsigned short* score_frame,unsigned short* score_frame_adjacent){
  int i,j;
  printk("################################################################\n");
  for(i=0; i<64; i++){
    printk("%2d",i);
    for(j=0; j<64;j++){

      int index = j+64*i+64+64*BLOB_LINE_OFFSET;
      int score = score_frame_adjacent[index];
      index = index%4096;
      //int score = score_frame[index];
      if(score <= 180)
	printk("  ");
      else if(score/10 > 99)
	printk("XX");
      else
	printk("%02d",score/10);
	//printk("00");
    }
    printk("\n");
  }
  printk("\n");
}



/* There is an offset of 96 bits
   Stage 1: get average on n = AVERAGE_COMPUTE_FRAME frames
   Stage 2: compute sigma on n = SIGMA_COMPUTE_FRAME frames
 */
static int normalize(unsigned char *current_frame,
		     unsigned short *score_frame,
		     unsigned short *score_frame_adjacent,
		     unsigned short *score_last_frame_adjacent,
		     unsigned short *average_frame,
		     unsigned short *sigma_frame,
		     int frame_index,
		     bool sigma_normalized,
		     bool average_computed,
		     struct touch_contact *touch_contacts){
  int retval, i, j, k, l,m, adj_index, index, contact_index, contact_match_index;
  bool cell_triggered;
  
  unsigned short sigma_threshold_factor;
  unsigned short sigma;
  unsigned short score;
  unsigned short current_value;
  unsigned short difference;
  struct touch_contact *contact;
  
  sigma_threshold_factor = SIGMA_THRESHOLD;
  contact_index = 0;
  retval = 0;

  
  for(i=0;i<64;i++){
    for(j=0;j<64;j++){
      index = j+i*64+64+64*BLOB_LINE_OFFSET;
      index = index%4096;
      if(!average_computed && frame_index < AVERAGE_COMPUTE_FRAME){
	if (frame_index == 0)
	  average_frame[index] = current_frame[index];
	else
	  average_frame[index] += current_frame[index];
	
	if(average_frame[index] > 65000){
	  printk("average is high %d %d %d %d\n", i, j, index, average_frame[index]);
	}
      }else if(!average_computed && frame_index == AVERAGE_COMPUTE_FRAME){
	//printk("average_computed %d %d %d %02X\n", i, j, index, value);
	average_frame[index] = average_frame[index]/AVERAGE_COMPUTE_FRAME;
      }else if(!sigma_normalized && frame_index < SIGMA_COMPUTE_FRAME + AVERAGE_COMPUTE_FRAME){
	current_value = current_frame[index];
	difference = current_value - average_frame[index];
	if(current_value < average_frame[index]){
	  difference = average_frame[index] - current_value;
	}
	//printk("average_computed %d %d %d %02X\n", difference, current_frame[index], average_frame[index], current_frame[index]);
	if(frame_index == AVERAGE_COMPUTE_FRAME+1)
	  sigma_frame[index] = difference;
	else
	  sigma_frame[index] += difference;

	if(sigma_frame[index] > 65000){
	  printk("sigma is high %d %d %d %d\n", i, j, index, average_frame[index]);
	}
	
      }else if(!sigma_normalized && frame_index == SIGMA_COMPUTE_FRAME + AVERAGE_COMPUTE_FRAME){
	//printk("1 sigma_computed %d \n", sigma_frame[index]);
	sigma_frame[index] = sigma_frame[index]/SIGMA_COMPUTE_FRAME;
	if(sigma_frame[index] < 1)
	  sigma_frame[index] = 1;
	//printk("2 sigma_computed %d \n", sigma_frame[index]);
      }else if(sigma_normalized && average_computed){
	current_value = current_frame[index];
	difference = current_value - average_frame[index];

	sigma = sigma_frame[index];
        score_frame_adjacent[index] = 0;

	if(current_value < average_frame[index]){ 
	  difference = average_frame[index] - current_value;
	}


	score_frame[index] = difference/sigma;

	/* compute top left cell with adjacent cells*/
	for(k=-2;k<1;k++){
	  for(l=-2;l<1;l++){
	    if(l+j>0 && k+i>0){
	      adj_index = (l+j)+(k+i)*64+64+64*BLOB_LINE_OFFSET;
	      score = score_frame[adj_index];
	      score_frame_adjacent[index-(65)] += score;
	    }
	  }
	}

	score_frame_adjacent[index] = (score_frame_adjacent[index] + score_last_frame_adjacent[index])/2;


	score = score_frame_adjacent[index];

	cell_triggered = false;
	if(score <= sigma_threshold_factor)
	  printk("  ");
	else if(score/10 > 99){
	  printk("XX");
	  cell_triggered = true;
	}else{
	  printk("%02d",score/10);
	  cell_triggered = true;
	}

	if(contact_index < MAX_CONTACTS){
	  if(cell_triggered){
	    /* is this point part of an other detected contact */
	    contact_match_index = -1;
	    for(m=0;m<contact_index;m++){
	      contact = &touch_contacts[m];
	      if(j >= contact->x -2 && j < contact->x + contact->w +3 &&
		 i >= contact->y -2 && i < contact->y + contact->h +3){
		contact_match_index = m;
		break;
	      }
	    }
	    
	    if(contact_match_index>-1){
	      contact = &touch_contacts[contact_match_index];
	      if(j - contact->x +1 > contact->w)
		contact->w = j - contact->x + 1;
	      if(i - contact->y +1> contact->h)
		contact->h = i - contact->y + 1;
	    }else{
	      contact_index++;
	      contact = &touch_contacts[contact_index];
	      contact->x = j;
	      contact->y = i;
	      contact->h = 1;
	      contact->w = 1;
	      //printk("%d %d %d %d --\n",contact->x, contact->y, contact->w, contact->h );
	    }
	    
	  }else{
	    
	  }
	}
	
	memcpy(score_last_frame_adjacent, score_frame_adjacent, 4160*2);
      }     
    }
    if(sigma_normalized && average_computed)
      printk("\n");
    
  }

  if(sigma_normalized && average_computed){
    printk("%d \n", contact_index);
    for(m=0;m<contact_index;m++){
      contact = &touch_contacts[m];
      printk("%02d %02d %02d %02d --\n",contact->x, contact->y, contact->w, contact->h );
    }
    //debug_matrix(score_frame, score_frame_adjacent);
  }

  return retval;
}

/* core function: poll for new input data */
static void skel_poll(struct input_polled_dev *polldev)
{
  struct input_dev *input = polldev->input;
  struct usb_skel *dev = polldev->private;
  int result, bulk_read, retval;
  
  result = usb_bulk_msg(dev->udev,
			usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			dev->bulk_in_size,
			&bulk_read,
			1000);

  if (result < 0) {
    printk("greentouch error in usb_bulk_read \n" );
    return;
  }

 
  /* Blob is a 64x64 octet matrix representing touch matrix prefixed by 64 
     unknown octets, normalization and threshold is required */
  retval = normalize(dev->bulk_in_buffer,
		     dev->score_frame,
		     dev->score_frame_adjacent,
		     dev->score_last_frame_adjacent,
		     dev->average_frame,
		     dev->sigma_frame,
		     dev->frame_index,
		     dev->sigma_normalized,
		     dev->average_computed,
		     dev->touch_contacts);

  if(!dev->sigma_normalized && dev->frame_index == SIGMA_COMPUTE_FRAME + AVERAGE_COMPUTE_FRAME){
    dev->sigma_normalized = true;
    printk("Sgima computed\n");
  }

  if(!dev->average_computed && dev->frame_index == AVERAGE_COMPUTE_FRAME){
    dev->average_computed = true;
    printk("Average computed\n");
  }
  
  if(dev->frame_index > CALIBRATE_EVERY){
    dev->frame_index = 0;
    dev->average_computed = false;
    dev->sigma_normalized = false;
    printk("Calibration relaunched\n");
  }

  //skel_report_inputs(dev->touches)
  
  input_mt_sync_frame(input);
  //printk("%s input_mt_sync_frame done\n", __func__);
  input_sync(input);
  //printk("%s input_sync done\n", __func__);
  dev->frame_index++;
}


/* Initialize input device parameters. */
static void input_setup(struct input_dev *input_dev)
{
        __set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
			     0, SENSOR_RES_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
			     0, SENSOR_RES_Y, 0, 0);

	input_set_abs_params(input_dev, ABS_MT_TOOL_X,
			     0, SENSOR_RES_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOOL_Y,
			     0, SENSOR_RES_Y, 0, 0);

	/* max value unknown, but major/minor axis
	 * can never be larger than screen */
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
			     0, SENSOR_RES_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR,
			     0, SENSOR_RES_Y, 0, 0);

	input_set_abs_params(input_dev, ABS_MT_ORIENTATION, 0, 1, 0, 0);

	input_mt_init_slots(input_dev, MAX_CONTACTS,
			    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
}

static int skel_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_skel *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct input_polled_dev *poll_dev;
	
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;
	
	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error;
	}
	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_usb_anchor(&dev->submitted);
	init_waitqueue_head(&dev->bulk_in_wait);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;
	dev->frame_index = 0;
	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!dev->bulk_in_endpointAddr &&
		    usb_endpoint_is_bulk_in(endpoint)) {
			/* we found a bulk in endpoint */
		        buffer_size = 4160;//usb_endpoint_maxp(endpoint);
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_buffer = kzalloc(buffer_size, GFP_KERNEL);
			dev->score_frame = kzalloc(buffer_size*2, GFP_KERNEL);
			dev->score_frame_adjacent = kzalloc(buffer_size*2, GFP_KERNEL);
			dev->score_last_frame_adjacent = kzalloc(buffer_size*2, GFP_KERNEL);
			dev->sigma_frame = kzalloc(buffer_size*2, GFP_KERNEL);
			dev->average_frame = kzalloc(buffer_size*2, GFP_KERNEL);
			
			
			if (!dev->bulk_in_buffer) {
				dev_err(&interface->dev,
					"Could not allocate bulk_in_buffer\n");
				goto error;
			}
			dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!dev->bulk_in_urb) {
				dev_err(&interface->dev,
					"Could not allocate bulk_in_urb\n");
				goto error;
			}
		}

		if (!dev->bulk_out_endpointAddr &&
		    usb_endpoint_is_bulk_out(endpoint)) {
			/* we found a bulk out endpoint */
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
		}
	}
	if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
		dev_err(&interface->dev,
			"Could not find both bulk-in and bulk-out endpoints\n");
		goto error;
	}

	poll_dev = input_allocate_polled_device();
	if (!poll_dev) {
		retval = -ENOMEM;
		printk("Error allocating polled dev");
		goto error;
	}
	/* Set up polled input device control structure */
	poll_dev->private = dev;
	poll_dev->poll_interval = POLL_INTERVAL;
	poll_dev->poll = skel_poll;

	input_setup(poll_dev->input);
        
	poll_dev->input->name = NAME_LONG;
	usb_to_input_id(dev->udev, &poll_dev->input->id);
	usb_make_path(dev->udev, dev->phys, sizeof(dev->phys));
	strlcat(dev->phys, "/input0", sizeof(dev->phys));

	poll_dev->input->phys = dev->phys;
	poll_dev->input->dev.parent = &interface->dev;
	
	dev->input = poll_dev;
	
	retval = input_register_polled_device(poll_dev);
	if (retval) {
	        printk("Error registering polled dev");
		dev_err(&interface->dev,
			"Unable to register polled input device.");
		goto error;
	}

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	//retval = usb_register_dev(interface, &skel_class);
	//if (retval) {
	//	/* something prevented us from registering this driver */
	//	dev_err(&interface->dev,
	//		"Not able to get a minor for this device.\n");
	//	usb_set_intfdata(interface, NULL);
	//	goto error;
	//}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB Skeleton device now attached to USBSkel-%d",
		 interface->minor);
	return 0;

error:
	if (dev)
		/* this frees allocated memory */
		kref_put(&dev->kref, skel_delete);
	return retval;
}

static void skel_disconnect(struct usb_interface *interface)
{
	struct usb_skel *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	input_unregister_polled_device(dev->input);
	input_free_polled_device(dev->input);
	
	/* give back our minor */
	usb_deregister_dev(interface, &skel_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->interface = NULL;
	mutex_unlock(&dev->io_mutex);

	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	kref_put(&dev->kref, skel_delete);

	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);
}

static void skel_draw_down(struct usb_skel *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int skel_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;
	skel_draw_down(dev);
	return 0;
}

static int skel_resume(struct usb_interface *intf)
{
	return 0;
}

static int skel_pre_reset(struct usb_interface *intf)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->io_mutex);
	skel_draw_down(dev);

	return 0;
}

static int skel_post_reset(struct usb_interface *intf)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

static struct usb_driver skel_driver = {
	.name =		"skeleton",
	.probe =	skel_probe,
	.disconnect =	skel_disconnect,
	.suspend =	skel_suspend,
	.resume =	skel_resume,
	.pre_reset =	skel_pre_reset,
	.post_reset =	skel_post_reset,
	.id_table =	skel_table,
	.supports_autosuspend = 1,
};

module_usb_driver(skel_driver);

MODULE_LICENSE("GPL");
